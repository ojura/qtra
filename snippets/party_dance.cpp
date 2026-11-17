// A separately releasable party layer. It consumes the existing audio.beat
// stream; it never owns an audio source, a player command or the guest panel.
#include "agent/agent_abi.h"
#include "cube_widget.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFont>
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
        cycle_ = cube->window()->property("partyDanceCycle").toInt() % 4;
        songKey_ = cube->window()->property("partyDanceSongKey").toString();
        songRevision_ = cube->window()->property("partyDanceSongRevision").toInt();
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
                {QStringLiteral("fpsLimit"), 30}, {QStringLiteral("particleLimit"), 24},
                {QStringLiteral("theme"), theme_}, {QStringLiteral("songKey"), songKey_},
                {QStringLiteral("songRevision"), songRevision_}};
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
        if (currentAct() == QStringLiteral("stadium")) drawStadium(p, w, h, u, t);
        else if (currentAct() == QStringLiteral("conga")) drawConga(p, w, h, u, t);
        else if (currentAct() == QStringLiteral("streamers")) drawStreamers(p, w, h, u, t);
        else drawDisco(p, w, h, u, t);
        if (currentAct() == QStringLiteral("disco") || currentAct() == QStringLiteral("streamers")) {
            drawFan(p, w * .25, h * .69, u * 1.6, .45 + .12 * std::sin(t * .7), 0, true, 1, t);
            drawFan(p, w * .75, h * .69, u * 1.6, .45 + .12 * std::sin(t * .7 + 1), 1, true, -1, t);
        }
        drawConfetti(p, w, h, u, t);
        ++frames_; drawMs_ = work.nsecsElapsed() / 1000000.0;
    }
private:
    QString currentAct() const
    {
        const QString acts[] = {QStringLiteral("disco"), QStringLiteral("streamers"), QStringLiteral("conga"), QStringLiteral("stadium")};
        return act_ == QStringLiteral("auto") ? acts[cycle_] : act_;
    }
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
            cycle_ = (cycle_ + 1) % 4; cube_->window()->setProperty("partyDanceCycle", cycle_);
        }
        if (anchor > 0) lastAct_ = anchor;
        const auto track = document.object().value(QStringLiteral("nowPlaying")).toObject();
        const QString title = track.value(QStringLiteral("title")).toString();
        if (!title.isEmpty()) {
            const QString key = track.value(QStringLiteral("uid")).toString() + QLatin1Char('|')
                + track.value(QStringLiteral("uri")).toString() + QLatin1Char('|') + title + QLatin1Char('|')
                + track.value(QStringLiteral("artist")).toString();
            if (key != songKey_) {
                songKey_ = key; ++songRevision_;
                cube_->window()->setProperty("partyDanceSongKey", songKey_);
                cube_->window()->setProperty("partyDanceSongRevision", songRevision_);
            }
            const QString words = title.toCaseFolded();
            if (words.contains(QStringLiteral("koktel"))) theme_ = QStringLiteral("cocktail");
            else if (words.contains(QStringLiteral("piti")) || words.contains(QStringLiteral("pivo")) || words.contains(QStringLiteral("vino"))) theme_ = QStringLiteral("bottles");
            else if (words.contains(QStringLiteral("gdje")) && words.contains(QStringLiteral("bio"))) theme_ = QStringLiteral("compass");
            else if (words.contains(QStringLiteral("masla"))) theme_ = QStringLiteral("dandelion");
            else if (words.contains(QStringLiteral("pepel"))) theme_ = QStringLiteral("embers");
            else {
                const QString defaults[] = {QStringLiteral("record"), QStringLiteral("speaker"), QStringLiteral("headphones"), QStringLiteral("microphone")};
                theme_ = defaults[songRevision_ % 4];
            }
        }
    }
    QColor color(int index) const
    {
        const QColor palette[] = {QColor("#63efff"), QColor("#ff6bc9"), QColor("#ffd166"), QColor("#8b81ff")};
        return palette[(index + songRevision_ + (cool_ ? 0 : 2)) % 4];
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
        for (int row = 0; row < 6; ++row) {
            const double a = std::pow(row / 6.0, 1.8), b = std::pow((row + 1) / 6.0, 1.8);
            const double y1 = horizon + (bottom - horizon) * a, y2 = horizon + (bottom - horizon) * b;
            for (int col = 0; col < 8; ++col) {
                const double left = (col - 4) / 4.0, right = (col - 3) / 4.0;
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
        for (int row = 0; row < 6; ++row) for (int col = 0; col < 14; ++col) {
            const double lat1 = -pi / 2 + row * pi / 6, lat2 = lat1 + pi / 6;
            const double lon1 = col * 2 * pi / 14 + t * .14, lon2 = lon1 + 2 * pi / 14;
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
    void drawProp(QPainter& p, QPointF hand, double scale, double t, bool toast = false)
    {
        p.save(); p.translate(hand); p.scale(scale, scale); p.rotate(3 * std::sin(t * .6));
        const QString theme = toast ? QStringLiteral("cocktail") : theme_;
        const QColor accent = color(0), ink("#0a1423"), white("#f2fbff");
        p.setPen(QPen(accent, 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.setBrush(ink);
        if (theme == QStringLiteral("bottles")) {
            p.setBrush(QColor("#087c68")); p.drawRoundedRect(QRectF(-29, -98, 58, 98), 13, 13);
            p.drawRoundedRect(QRectF(-12, -139, 24, 48), 5, 5);
            p.setBrush(accent); p.drawRoundedRect(QRectF(-16, -148, 32, 13), 4, 4);
            p.setPen(Qt::NoPen); p.setBrush(white); p.drawRoundedRect(QRectF(-22, -72, 44, 38), 5, 5);
            p.setBrush(ink); p.drawEllipse(QPointF(0, -53), 11, 11);
        } else if (theme == QStringLiteral("cocktail")) {
            p.setBrush(opacity(accent, .65)); p.drawPolygon(QPolygonF{QPointF(-60, -123), QPointF(60, -123), QPointF(0, -56)});
            p.drawLine(QPointF(0, -56), QPointF(0, -8)); p.drawLine(QPointF(-35, -8), QPointF(35, -8));
            p.setPen(QPen(white, 7, Qt::SolidLine, Qt::RoundCap)); p.drawLine(QPointF(22, -92), QPointF(47, -162));
            p.setPen(QPen(QColor("#a7ff85"), 5)); p.setBrush(QColor("#53933a")); p.drawEllipse(QPointF(-44, -123), 20, 20);
        } else if (theme == QStringLiteral("compass")) {
            p.drawEllipse(QPointF(0, -76), 64, 64);
            p.setPen(QPen(white, 5));
            for (int direction = 0; direction < 4; ++direction) {
                const double angle = direction * pi / 2;
                p.drawLine(QPointF(std::sin(angle) * 45, -76 + std::cos(angle) * 45), QPointF(std::sin(angle) * 55, -76 + std::cos(angle) * 55));
            }
            p.setPen(Qt::NoPen); p.setBrush(QColor("#ff795e")); p.drawPolygon(QPolygonF{QPointF(0, -129), QPointF(-16, -70), QPointF(15, -76)});
            p.setBrush(QColor("#75e7ff")); p.drawPolygon(QPolygonF{QPointF(0, -25), QPointF(-16, -70), QPointF(15, -76)});
        } else if (theme == QStringLiteral("dandelion")) {
            p.setPen(QPen(QColor("#88d88b"), 7, Qt::SolidLine, Qt::RoundCap)); p.drawLine(QPointF(0, 0), QPointF(0, -140));
            p.setBrush(QColor("#88d88b")); p.drawEllipse(QRectF(-42, -68, 40, 18)); p.drawEllipse(QRectF(2, -93, 40, 18));
            p.setPen(QPen(white, 4, Qt::SolidLine, Qt::RoundCap));
            for (int seed = 0; seed < 12; ++seed) {
                const double a = seed * 2 * pi / 12;
                const QPointF end(std::sin(a) * 54, -143 + std::cos(a) * 54);
                p.drawLine(QPointF(0, -143), end); p.drawLine(end + QPointF(-8, -5), end + QPointF(8, 5));
                p.drawLine(end + QPointF(5, -8), end + QPointF(-5, 8));
            }
            p.setPen(Qt::NoPen); p.setBrush(QColor("#ffe797")); p.drawEllipse(QPointF(0, -143), 10, 10);
        } else if (theme == QStringLiteral("embers")) {
            QPainterPath flame; flame.moveTo(0, 0); flame.cubicTo(-73, -32, -55, -91, -28, -143);
            flame.cubicTo(-20, -100, 8, -103, 12, -162); flame.cubicTo(80, -91, 76, -22, 0, 0);
            p.setPen(QPen(QColor("#ffc575"), 5)); p.setBrush(QColor("#ec5b38")); p.drawPath(flame);
            p.scale(.48, .65); p.setPen(Qt::NoPen); p.setBrush(QColor("#ffde72")); p.drawPath(flame);
        } else if (theme == QStringLiteral("speaker")) {
            p.drawRoundedRect(QRectF(-48, -144, 96, 144), 11, 11);
            p.setBrush(opacity(accent, .4)); p.drawEllipse(QPointF(0, -99), 28, 28); p.drawEllipse(QPointF(0, -37), 31, 31);
            p.setBrush(accent); p.drawEllipse(QPointF(0, -37), 11, 11);
        } else if (theme == QStringLiteral("headphones")) {
            QPainterPath band; band.moveTo(-55, -30); band.cubicTo(-84, -167, 84, -167, 55, -30);
            p.setBrush(Qt::NoBrush); p.setPen(QPen(accent, 13, Qt::SolidLine, Qt::RoundCap)); p.drawPath(band);
            p.setBrush(ink); p.setPen(QPen(white, 5)); p.drawRoundedRect(QRectF(-72, -66, 33, 66), 10, 10); p.drawRoundedRect(QRectF(39, -66, 33, 66), 10, 10);
        } else if (theme == QStringLiteral("microphone")) {
            p.setPen(QPen(accent, 20, Qt::SolidLine, Qt::RoundCap)); p.drawLine(QPointF(0, -5), QPointF(20, -93));
            p.setPen(QPen(white, 5)); p.setBrush(ink); p.drawEllipse(QRectF(-15, -165, 70, 86));
            p.drawLine(QPointF(-7, -127), QPointF(47, -127)); p.drawLine(QPointF(-6, -111), QPointF(45, -111));
        } else {
            p.drawEllipse(QPointF(0, -74), 64, 64); p.setPen(QPen(white, 3)); p.drawEllipse(QPointF(0, -74), 45, 45);
            p.setPen(Qt::NoPen); p.setBrush(accent); p.drawEllipse(QPointF(0, -74), 24, 24);
            p.setBrush(ink); p.drawEllipse(QPointF(0, -74), 5, 5);
        }
        p.restore();
    }
    void drawFan(QPainter& p, double x, double y, double scale, double lift, int id, bool prop, int handSide, double t)
    {
        p.save(); p.translate(x, y); p.scale(scale, scale);
        const QColor accent = color(id + 1); const double bounce = 10 * lift + pulse_;
        p.setPen(QPen(accent, 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (int side : {-1, 1}) {
            p.drawPolyline(QPolygonF{QPointF(side * 13, -31 - bounce), QPointF(side * 21, -13), QPointF(side * 27, 0)});
            p.drawLine(QPointF(side * 27 - 9, 0), QPointF(side * 27 + 9, 0));
            const QPointF hand(side * (prop && side == handSide ? 78 : 43), -61 - 43 * lift - bounce);
            p.drawPolyline(QPolygonF{QPointF(side * 24, -67 - bounce), QPointF(side * 37, -56 - 24 * lift - bounce), hand});
            p.setBrush(accent); p.drawEllipse(hand, 5, 5);
        }
        p.translate(0, -bounce); p.setBrush(QColor("#0b1829")); p.setPen(QPen(accent, 3.5));
        p.drawRoundedRect(QRectF(-27, -80, 54, 49), 8, 8); p.drawRoundedRect(QRectF(-33, -121, 66, 35), 10, 10);
        p.setBrush(opacity(accent, .45)); p.drawEllipse(QPointF(0, -56), 16, 16);
        p.setPen(Qt::NoPen); p.setBrush(QColor("#e4fcff")); p.drawEllipse(QPointF(-12, -105), 4.5, 6); p.drawEllipse(QPointF(12, -105), 4.5, 6);
        p.setPen(QPen(QColor("#e4fcff"), 3, Qt::SolidLine, Qt::RoundCap)); p.drawArc(QRectF(-9, -105, 18, 15), 205 * 16, 130 * 16);
        if (prop) drawProp(p, QPointF(handSide * 78, -61 - 43 * lift), 1.35, t, handSide < 0);
        p.restore();
    }
    void drawStadium(QPainter& p, double w, double h, double u, double t)
    {
        for (int row = 0; row < 2; ++row) for (int col = 0; col < 4; ++col) {
            const double lift = std::pow(std::max(0.0, std::cos(t * .65 - col * .8 - row * .35)), 3);
            const bool prop = row == 1 && (col == 1 || col == 2);
            drawFan(p, w * (.17 + col * .22), h * (.51 + row * .24), u * (1.05 + row * .25), lift, row * 4 + col,
                    prop, col < 2 ? 1 : -1, t);
        }
        const double phase = std::fmod(t, 22.0);
        if (phase >= 1 && phase < 5) {
            p.save(); p.setOpacity(std::clamp(std::min((phase - 1) / .7, (5 - phase) / .7), 0.0, 1.0));
            const QRectF card(w * .5 - 140 * u, h * .27 - 47 * u, 280 * u, 94 * u);
            p.setPen(QPen(color(2), 4 * u)); p.setBrush(QColor("#0b1829")); p.drawRoundedRect(card, 14 * u, 14 * u);
            QFont font(QStringLiteral("DejaVu Sans")); font.setPixelSize(qRound(46 * u)); font.setBold(true); p.setFont(font);
            p.setPen(QColor("#f7fcff")); p.drawText(card, Qt::AlignCenter, static_cast<int>(t / 22) % 2 ? QStringLiteral("BIS!") : QStringLiteral("OPA!"));
            p.restore();
        }
    }
    void drawConga(QPainter& p, double w, double h, double u, double t)
    {
        QPainterPath trail;
        for (int point = 0; point <= 64; ++point) {
            const double x = point / 64.0;
            const double y = h * (.63 + .07 * std::sin((x + .15) / 1.3 * 2 * pi + t * .20));
            point ? trail.lineTo(x * w, y + 8 * u) : trail.moveTo(x * w, y + 8 * u);
        }
        p.setBrush(Qt::NoBrush); p.setPen(QPen(opacity(color(0), .23), 2 * u, Qt::DashLine)); p.drawPath(trail);
        for (int robot = 5; robot >= 0; --robot) {
            const double progress = fraction(.8 + t * .018 - robot * .11);
            const double x = w * (progress * 1.3 - .15);
            const double y = h * (.63 + .07 * std::sin(progress * 2 * pi + t * .20));
            const double gait = std::sin(t * 3.2 - robot * .64);
            const double bounce = 4 * std::abs(gait) + 2 * pulse_;
            const QColor accent = color(robot);
            p.save(); p.translate(x, y); p.scale(u * 1.45, u * 1.45);
            p.setPen(Qt::NoPen); p.setBrush(QColor(0, 0, 0, 70)); p.drawEllipse(QRectF(-38, -1, 76, 10));
            p.setPen(QPen(opacity(accent, .88), 6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            for (int side : {-1, 1}) {
                const double step = side * gait;
                const QPointF hip(side * 13, -36 - bounce);
                const QPointF knee(side * (20 + 6 * step), -17 - 3 * step);
                const QPointF foot(side * (22 - 7 * step), -2 - std::max(0.0, step) * 8);
                p.drawPolyline(QPolygonF{hip, knee, foot});
                p.drawLine(foot - QPointF(7, 0), foot + QPointF(9, 0));
                const QPointF shoulder(side * 24, -75 - bounce);
                const QPointF elbow(side * 41, -69 - bounce + 9 * step);
                const bool waving = robot % 3 == 1 && side == -1;
                const bool holding = (robot == 0 || robot == 3) && side == 1;
                const QPointF hand(side * (holding ? 80 : 59), (waving ? -116 : -82) - bounce + 5 * step);
                p.drawPolyline(QPolygonF{shoulder, elbow, hand});
                p.setBrush(opacity(accent, .95)); p.drawEllipse(hand, 5, 5);
            }
            p.translate(0, -bounce);
            p.setPen(QPen(opacity(accent, .9), 2.5)); p.setBrush(QColor(13, 22, 36, 235));
            p.drawRoundedRect(QRectF(-25, -88, 50, 53), 9, 9);
            p.setBrush(opacity(accent, .15)); p.drawEllipse(QPointF(0, -61), 16, 16);
            p.setBrush(opacity(accent, .65)); p.drawEllipse(QPointF(0, -61), 6 + 2 * pulse_, 6 + 2 * pulse_);
            if (robot == 0) {
                p.setBrush(QColor(8, 13, 25, 245)); p.drawEllipse(QPointF(0, -113), 34, 34);
                p.setBrush(Qt::NoBrush); p.setPen(QPen(opacity(accent, .35), 1));
                for (int ring = 0; ring < 3; ++ring) p.drawEllipse(QPointF(0, -113), 20 + ring * 4, 20 + ring * 4);
                p.setPen(Qt::NoPen); p.setBrush(color(1)); p.drawEllipse(QPointF(0, -113), 14, 14);
                p.setBrush(color(2)); p.drawPolygon(QPolygonF{QPointF(-13, -146), QPointF(0, -169), QPointF(13, -146)});
            } else {
                p.setPen(QPen(opacity(accent, .9), 2.5)); p.setBrush(QColor(13, 22, 36, 245));
                p.drawRoundedRect(QRectF(-30, -132, 60, 39), 10, 10);
                p.drawLine(QPointF(0, -132), QPointF(5 * gait, -145));
                p.setBrush(accent); p.drawEllipse(QPointF(5 * gait, -148), 4, 4);
            }
            p.setPen(Qt::NoPen); p.setBrush(QColor("#ccfbff"));
            p.drawEllipse(QPointF(-11, -115), 4, 6); p.drawEllipse(QPointF(11, -115), 4, 6);
            p.setBrush(Qt::NoBrush); p.setPen(QPen(QColor("#ccfbff"), 2));
            p.drawArc(QRectF(-8, -115, 16, 16), 205 * 16, 130 * 16);
            if (robot == 0 || robot == 3) drawProp(p, QPointF(80, -82 + 5 * gait), 1.35, t, robot == 3);
            p.restore();
        }
    }
    void drawConfetti(QPainter& p, double w, double h, double u, double t)
    {
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 24; ++i) {
            const double age = fraction(i * .61803398875 + t * (.025 + (i % 5) * .002));
            const double fade = std::sin(age * pi);
            const double x = w * fraction(i * .754877666 + .027 * std::sin(t * .3 + i));
            const double y = h * (1.08 - age * .88);
            p.save(); p.translate(x, y); p.rotate(t * (12 + i % 9) + i * 37);
            p.setBrush(opacity(color(i), fade * (.24 + .17 * energy_ + .12 * pulse_)));
            p.drawRoundedRect(QRectF(-4.5 * u, -12 * u, 9 * u, (18 + i % 6) * u), u, u); p.restore();
        }
    }
    QPointer<CubeWidget> cube_;
    QTimer frame_, poll_; QElapsedTimer clock_, frameClock_; QLocalSocket socket_;
    QByteArray buffer_, bytes_; QString stateFile_, socketPath_, audioError_, stateError_, act_, songKey_, theme_ = QStringLiteral("record");
    quint64 frames_ = 0, beats_ = 0; qint64 lastAct_ = 0;
    int cycle_ = 0, songRevision_ = 0; double pulse_ = 0, bpm_ = 0, drawMs_ = 0, energy_ = .7; bool cool_ = true;
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
        if (act != QStringLiteral("auto") && act != QStringLiteral("disco") && act != QStringLiteral("streamers") && act != QStringLiteral("conga") && act != QStringLiteral("stadium")) {
            host->fail(host->invocation_context, "act must be auto, disco, streamers, conga or stadium"); return;
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
