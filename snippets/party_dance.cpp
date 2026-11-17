// A separately releasable party layer. It consumes the existing audio.beat
// stream; it never owns an audio source, a player command or the guest panel.
#include "agent/agent_abi.h"
#include "cube_widget.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace {
constexpr double pi = 3.14159265358979323846;
double fraction(double value) { return value - std::floor(value); }
QColor opacity(QColor color, double value) { color.setAlphaF(std::clamp(value, 0.0, 1.0)); return color; }

class PartyDance final : public QWidget {
public:
    explicit PartyDance(CubeWidget* cube) : QWidget(cube), cube_(cube)
    {
        setObjectName(QStringLiteral("partyDance")); setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_TransparentForMouseEvents); setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false); setGeometry(cube->rect()); cube->installEventFilter(this);
        cycle_ = cube->window()->property("partyDanceCycle").toInt() % 2;
        act_ = cube->window()->property("partyDanceAct").toString();
        if (act_.isEmpty()) act_ = QStringLiteral("auto");
        clock_.start(); frameClock_.start(); frame_.setInterval(34); frame_.setTimerType(Qt::PreciseTimer);
        connect(&frame_, &QTimer::timeout, this, [this] {
            const double dt = std::clamp(frameClock_.restart() / 1000.0, 0.0, .1);
            pulse_ *= std::exp(-dt / .55); update();
        });
        connect(&socket_, &QLocalSocket::connected, this, [this] {
            audioError_.clear();
            socket_.write("{\"id\":1,\"command\":\"event.subscribe\",\"params\":{\"prefixes\":[\"audio.beat\"]}}\n");
        });
        connect(&socket_, &QLocalSocket::readyRead, this, [this] { readBeats(); });
        connect(&socket_, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
            audioError_ = socket_.errorString();
        });
        poll_.setInterval(500);
        connect(&poll_, &QTimer::timeout, this, [this] { readState(); connectAudio(); });
        poll_.start(); show(); stackBelowStage();
    }
    ~PartyDance() override { if (cube_) cube_->removeEventFilter(this); socket_.abort(); }

    void configure(const QJsonObject& request)
    {
        if (request.contains(QStringLiteral("stateFile"))) {
            stateFile_ = request.value(QStringLiteral("stateFile")).toString(); bytes_.clear();
        }
        if (request.contains(QStringLiteral("agentSocket"))) {
            const auto path = request.value(QStringLiteral("agentSocket")).toString();
            if (path != socketPath_) { socket_.abort(); buffer_.clear(); socketPath_ = path; }
        }
        if (request.contains(QStringLiteral("act"))) {
            act_ = request.value(QStringLiteral("act")).toString();
            cube_->window()->setProperty("partyDanceAct", act_);
        }
        readState(); connectAudio(); stackBelowStage(); update();
    }
    QJsonObject status() const
    {
        return {{QStringLiteral("active"), true}, {QStringLiteral("act"), currentAct()},
                {QStringLiteral("autoActs"), act_ == QStringLiteral("auto")},
                {QStringLiteral("frames"), static_cast<qint64>(frames_)},
                {QStringLiteral("beats"), static_cast<qint64>(beats_)}, {QStringLiteral("pulse"), pulse_},
                {QStringLiteral("bpm"), bpm_}, {QStringLiteral("drawMs"), drawMs_},
                {QStringLiteral("audioConnected"), socket_.state() == QLocalSocket::ConnectedState},
                {QStringLiteral("audioError"), audioError_}, {QStringLiteral("stateError"), stateError_},
                {QStringLiteral("width"), width()}, {QStringLiteral("height"), height()},
                {QStringLiteral("parentWidth"), cube_ ? cube_->width() : 0},
                {QStringLiteral("fpsLimit"), 30}, {QStringLiteral("particleLimit"), 48}};
    }
protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (object == cube_ && event->type() == QEvent::Resize) {
            setGeometry(cube_->rect()); stackBelowStage();
        }
        return QWidget::eventFilter(object, event);
    }
    void showEvent(QShowEvent* event) override { frameClock_.restart(); frame_.start(); QWidget::showEvent(event); }
    void hideEvent(QHideEvent* event) override { frame_.stop(); QWidget::hideEvent(event); }
    void paintEvent(QPaintEvent*) override
    {
        QElapsedTimer work; work.start();
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const double w = width(), h = height(), u = std::min(w / 1440.0, h / 1000.0);
        const double t = clock_.elapsed() / 1000.0;
        if (w < 2 || h < 2) return;
        if (currentAct() == QStringLiteral("streamers")) drawStreamers(p, w, h, u, t);
        else drawDisco(p, w, h, u, t);
        drawConfetti(p, w, h, u, t);
        ++frames_; drawMs_ = work.nsecsElapsed() / 1000000.0;
    }
private:
    QString currentAct() const { return act_ == QStringLiteral("auto") ? (cycle_ ? QStringLiteral("streamers") : QStringLiteral("disco")) : act_; }
    void stackBelowStage()
    {
        if (cube_) if (auto* stage = cube_->findChild<QWidget*>(QStringLiteral("partyStage"), Qt::FindDirectChildrenOnly)) stackUnder(stage);
    }
    void connectAudio()
    {
        if (!socketPath_.isEmpty() && socket_.state() == QLocalSocket::UnconnectedState) socket_.connectToServer(socketPath_);
    }
    void readBeats()
    {
        buffer_ += socket_.readAll();
        if (buffer_.size() > 65536) { buffer_.clear(); audioError_ = QStringLiteral("Oversized audio event"); return; }
        qsizetype newline;
        while ((newline = buffer_.indexOf('\n')) >= 0) {
            const auto message = QJsonDocument::fromJson(buffer_.left(newline)).object(); buffer_.remove(0, newline + 1);
            if (message.value(QStringLiteral("event")).toString() != QStringLiteral("audio.beat")) continue;
            const auto data = message.value(QStringLiteral("data")).toObject();
            const auto count = static_cast<quint64>(data.value(QStringLiteral("count")).toDouble());
            if (count == beats_) continue;
            beats_ = count;
            const double strength = data.value(QStringLiteral("strength")).toDouble();
            if (std::isfinite(strength)) pulse_ = std::max(pulse_, std::clamp(strength, 0.0, 1.5));
            const double bpm = data.value(QStringLiteral("bpm")).toDouble();
            if (std::isfinite(bpm)) bpm_ = std::clamp(bpm, 0.0, 240.0);
        }
    }
    void readState()
    {
        if (stateFile_.isEmpty()) return;
        QFile file(stateFile_);
        if (!file.open(QIODevice::ReadOnly)) { stateError_ = file.errorString(); return; }
        const auto bytes = file.read(1024 * 1024 + 1);
        if (bytes.size() > 1024 * 1024) { stateError_ = QStringLiteral("State exceeds 1 MiB"); return; }
        if (bytes == bytes_) return;
        QJsonParseError error; const auto document = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) { stateError_ = QStringLiteral("Invalid party state"); return; }
        bytes_ = bytes; stateError_.clear();
        const auto scene = document.object().value(QStringLiteral("scene")).toObject();
        energy_ = std::clamp(scene.value(QStringLiteral("energy")).toDouble(.7), 0.0, 1.0);
        cool_ = scene.value(QStringLiteral("palette")).toString() == QStringLiteral("ice");
        const auto anchor = static_cast<qint64>(scene.value(QStringLiteral("audioActStartedAt")).toDouble());
        if (anchor > 0 && lastAct_ > 0 && anchor != lastAct_) {
            cycle_ = (cycle_ + 1) % 2; cube_->window()->setProperty("partyDanceCycle", cycle_);
        }
        if (anchor > 0) lastAct_ = anchor;
    }
    QColor color(int index) const
    {
        const QColor palette[] = {QColor("#63efff"), QColor("#ff6bc9"), QColor("#ffd166"), QColor("#8b81ff")};
        return palette[(index + (cool_ ? 0 : 2)) % 4];
    }
    void drawDisco(QPainter& p, double w, double h, double u, double t)
    {
        const QPointF ball(w * .5, h * .36); const double radius = 105 * u;
        const double horizon = h * .59, bottom = h * 1.03;
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 4; ++i) {
            const double end = w * (.12 + i * .255 + .045 * std::sin(t * .13 + i));
            QLinearGradient light(ball, QPointF(end, bottom));
            light.setColorAt(0, opacity(color(i), .02)); light.setColorAt(1, opacity(color(i), .11 + .025 * pulse_));
            p.setBrush(light); p.drawPolygon(QPolygonF{ball + QPointF(-radius * .4, 0), ball + QPointF(radius * .4, 0),
                QPointF(end + w * .11, bottom), QPointF(end - w * .11, bottom)});
        }
        for (int row = 0; row < 9; ++row) {
            const double a = std::pow(row / 9.0, 1.8), b = std::pow((row + 1) / 9.0, 1.8);
            const double y1 = horizon + (bottom - horizon) * a, y2 = horizon + (bottom - horizon) * b;
            for (int col = 0; col < 12; ++col) {
                const double left = (col - 6) / 6.0, right = (col - 5) / 6.0;
                const QPolygonF tile{QPointF(w * .5 + left * w * (.11 + .48 * a), y1),
                    QPointF(w * .5 + right * w * (.11 + .48 * a), y1),
                    QPointF(w * .5 + right * w * (.11 + .48 * b), y2),
                    QPointF(w * .5 + left * w * (.11 + .48 * b), y2)};
                const QColor shade = color(col + row);
                p.setBrush(opacity(shade, .045 + .035 * (1 + std::sin(t * .65 + col * .7 + row)) + .045 * pulse_));
                p.setPen(QPen(opacity(shade, .25 + .09 * pulse_), 1.1 * u)); p.drawPolygon(tile);
            }
        }
        p.setPen(QPen(QColor(160, 190, 210, 65), 1.4 * u)); p.drawLine(QPointF(ball.x(), 0), ball - QPointF(0, radius));
        p.setPen(Qt::NoPen); p.setBrush(QColor(15, 21, 35, 210)); p.drawEllipse(ball, radius, radius);
        for (int row = 0; row < 8; ++row) for (int col = 0; col < 20; ++col) {
            const double lat1 = -pi / 2 + row * pi / 8, lat2 = lat1 + pi / 8;
            const double lon1 = col * 2 * pi / 20 + t * .14, lon2 = lon1 + 2 * pi / 20;
            const double face = std::cos((lon1 + lon2) / 2);
            if (face <= 0) continue;
            auto point = [&](double lat, double lon) { return ball + QPointF(std::cos(lat) * std::sin(lon), std::sin(lat)) * radius; };
            const double shine = .27 + .55 * face * std::cos((lat1 + lat2) / 2);
            QColor tile = ((row + col) % 5 == 0) ? color(row + col) : QColor::fromRgbF(shine * .8, shine * .91, shine);
            p.setBrush(opacity(tile, .83)); p.setPen(QPen(QColor(12, 18, 32, 145), .9 * u));
            p.drawPolygon(QPolygonF{point(lat1, lon1), point(lat1, lon2), point(lat2, lon2), point(lat2, lon1)});
        }
        p.setBrush(Qt::NoBrush); p.setPen(QPen(opacity(color(0), .55), 2 * u)); p.drawEllipse(ball, radius, radius);
    }
    void drawStreamers(QPainter& p, double w, double h, double u, double t)
    {
        for (int ribbon = 0; ribbon < 7; ++ribbon) {
            QPainterPath path;
            for (int point = 0; point <= 48; ++point) {
                const double a = point / 48.0;
                const double x = w * (a * 1.2 - .1);
                const double y = h * (.51 + .18 * std::sin(a * pi * 2 + t * .2 + ribbon * .34)) + ribbon * 13 * u;
                point ? path.lineTo(x, y) : path.moveTo(x, y);
            }
            p.setBrush(Qt::NoBrush); p.setPen(QPen(opacity(color(ribbon), .12), (11 + 2 * pulse_) * u, Qt::SolidLine, Qt::RoundCap)); p.drawPath(path);
            p.setPen(QPen(opacity(color(ribbon), .60), 2.7 * u, Qt::SolidLine, Qt::RoundCap)); p.drawPath(path);
        }
        for (int record = 0; record < 5; ++record) {
            const double phase = t * .16 + record * 2 * pi / 5;
            const QPointF center(w * (.5 + .29 * std::cos(phase)), h * (.58 + .14 * std::sin(phase)));
            const double r = (43 + 9 * std::sin(phase) + 3 * pulse_) * u;
            p.setPen(QPen(opacity(color(record), .8), 2 * u)); p.setBrush(QColor(10, 15, 28, 220)); p.drawEllipse(center, r, r);
            p.setBrush(Qt::NoBrush); p.setPen(QPen(QColor(120, 150, 180, 65), u));
            for (int groove = 1; groove < 4; ++groove) p.drawEllipse(center, r * (.35 + groove * .14), r * (.35 + groove * .14));
            p.setPen(Qt::NoPen); p.setBrush(color(record)); p.drawEllipse(center, r * .24, r * .24);
            p.setBrush(QColor("#101522")); p.drawEllipse(center, 3 * u, 3 * u);
        }
    }
    void drawConfetti(QPainter& p, double w, double h, double u, double t)
    {
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 48; ++i) {
            const double age = fraction(i * .61803398875 + t * (.025 + (i % 5) * .002));
            const double fade = std::sin(age * pi);
            const double x = w * fraction(i * .754877666 + .027 * std::sin(t * .3 + i));
            const double y = h * (1.08 - age * .88);
            p.save(); p.translate(x, y); p.rotate(t * (12 + i % 9) + i * 37);
            p.setBrush(opacity(color(i), fade * (.24 + .17 * energy_ + .12 * pulse_)));
            p.drawRoundedRect(QRectF(-3 * u, -9 * u, 6 * u, (12 + i % 6) * u), u, u); p.restore();
        }
    }
    QPointer<CubeWidget> cube_;
    QTimer frame_, poll_; QElapsedTimer clock_, frameClock_; QLocalSocket socket_;
    QByteArray buffer_, bytes_; QString stateFile_, socketPath_, audioError_, stateError_, act_;
    quint64 frames_ = 0, beats_ = 0; qint64 lastAct_ = 0;
    int cycle_ = 0; double pulse_ = 0, bpm_ = 0, drawMs_ = 0, energy_ = .7; bool cool_ = true;
};

QPointer<PartyDance> dance;
void release(const RuntimeAgentHost* host)
{
    if (!host || host->abi_version != RUNTIME_AGENT_ABI) return;
    delete dance.data(); host->complete_json(host->invocation_context, "{\"removed\":true}");
}
void run(const RuntimeAgentHost* host)
{
    if (!host || host->abi_version != RUNTIME_AGENT_ABI) return;
    const auto request = QJsonDocument::fromJson(QByteArray(host->request_json(host->invocation_context))).object();
    auto* cube = static_cast<CubeWidget*>(host->find_qobject(host->agent_context, "cubeView"));
    if (!cube || QThread::currentThread() != cube->thread()) { host->fail(host->invocation_context, "Use the GUI executor with cubeView available"); return; }
    if (request.value(QStringLiteral("restore")).toBool()) { release(host); return; }
    if (request.contains(QStringLiteral("act"))) {
        const QString act = request.value(QStringLiteral("act")).toString();
        if (act != QStringLiteral("auto") && act != QStringLiteral("disco") && act != QStringLiteral("streamers")) {
            host->fail(host->invocation_context, "act must be auto, disco or streamers"); return;
        }
    }
    if (!dance) {
        if (cube->findChild<QWidget*>(QStringLiteral("partyDance"), Qt::FindDirectChildrenOnly)) {
            host->fail(host->invocation_context, "Release the previous party dance module first"); return;
        }
        dance = new PartyDance(cube);
    }
    if (!request.isEmpty()) dance->configure(request);
    host->complete_json(host->invocation_context, QJsonDocument(dance->status()).toJson(QJsonDocument::Compact).constData());
}
const RuntimeAgentSnippet descriptor{RUNTIME_AGENT_ABI, sizeof(RuntimeAgentSnippet), "LARICS procedural party dance", &run, &release};
}
extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippet* runtime_agent_snippet_init() { return &descriptor; }
