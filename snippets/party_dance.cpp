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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr double pi = 3.14159265358979323846;
double fraction(double value) { return value - std::floor(value); }
QColor opacity(QColor color, double value) { color.setAlphaF(std::clamp(value, 0.0, 1.0)); return color; }
const QStringList& danceActs()
{
    static const QStringList acts{QStringLiteral("disco"), QStringLiteral("streamers"), QStringLiteral("conga"),
        QStringLiteral("stadium"), QStringLiteral("kolo"), QStringLiteral("tram")};
    return acts;
}
bool supportedTheme(const QString& theme)
{
    static const QStringList themes{QStringLiteral("record"), QStringLiteral("speaker"), QStringLiteral("headphones"),
        QStringLiteral("microphone"), QStringLiteral("bottles"), QStringLiteral("cocktail"), QStringLiteral("compass"),
        QStringLiteral("dandelion"), QStringLiteral("embers")};
    return themes.contains(theme);
}

class PartyDance final : public QWidget {
public:
    explicit PartyDance(CubeWidget* cube) : QWidget(cube), cube_(cube)
    {
        setObjectName(QStringLiteral("partyDance")); setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_TransparentForMouseEvents); setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false); setGeometry(cube->rect()); cube->installEventFilter(this);
        cycle_ = std::max(0, cube->window()->property("partyDanceCycle").toInt()) % danceActs().size();
        const qint64 storedInterval = cube->window()->property("partyDanceFormIntervalMs").toLongLong();
        if (storedInterval >= 1000 && storedInterval <= 600000) formIntervalMs_ = storedInterval;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        formStartedAt_ = cube->window()->property("partyDanceFormStartedAt").toLongLong();
        if (formStartedAt_ <= 0 || formStartedAt_ > now) formStartedAt_ = now;
        songKey_ = cube->window()->property("partyDanceSongKey").toString();
        songRevision_ = cube->window()->property("partyDanceSongRevision").toInt();
        const QString automatic = cube->window()->property("partyDanceAutomaticTheme").toString();
        if (supportedTheme(automatic)) automaticTheme_ = automatic;
        theme_ = automaticTheme_;
        const QString effective = cube->window()->property("partyDanceTheme").toString();
        if (supportedTheme(effective)) {
            theme_ = effective;
            if (cube->window()->property("partyDanceThemeSource").toString() == QStringLiteral("cue")) themeSource_ = QStringLiteral("cue");
        }
        act_ = cube->window()->property("partyDanceAct").toString();
        if (act_ != QStringLiteral("auto") && !danceActs().contains(act_)) act_ = QStringLiteral("auto");
        saveFormCycle();
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
        connect(&poll_, &QTimer::timeout, this, [this] { rotateForms(); readState(); connectAudio(); });
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
            if (act_ != QStringLiteral("auto")) cycle_ = danceActs().indexOf(act_);
            cube_->window()->setProperty("partyDanceAct", act_);
        }
        if (request.contains(QStringLiteral("startAct"))) cycle_ = danceActs().indexOf(request.value(QStringLiteral("startAct")).toString());
        if (request.contains(QStringLiteral("formSeconds"))) formIntervalMs_ = qRound64(request.value(QStringLiteral("formSeconds")).toDouble() * 1000);
        if (request.contains(QStringLiteral("act")) || request.contains(QStringLiteral("startAct")) || request.contains(QStringLiteral("formSeconds"))) {
            formStartedAt_ = QDateTime::currentMSecsSinceEpoch(); saveFormCycle();
        }
        readState(); connectAudio(); stackBelowStage(); update();
    }
    QJsonObject status() const
    {
        return {{QStringLiteral("active"), true}, {QStringLiteral("act"), currentAct()},
                {QStringLiteral("autoActs"), act_ == QStringLiteral("auto")},
                {QStringLiteral("availableActs"), QJsonArray::fromStringList(danceActs())},
                {QStringLiteral("formIntervalMs"), formIntervalMs_},
                {QStringLiteral("nextActAt"), act_ == QStringLiteral("auto") ? formStartedAt_ + formIntervalMs_ : 0},
                {QStringLiteral("formation"), currentAct() == QStringLiteral("kolo") ? QStringLiteral("closed-ring")
                    : currentAct() == QStringLiteral("tram") ? QStringLiteral("tram-passengers") : currentAct()},
                {QStringLiteral("frames"), static_cast<qint64>(frames_)},
                {QStringLiteral("beats"), static_cast<qint64>(beats_)}, {QStringLiteral("pulse"), pulse_},
                {QStringLiteral("bpm"), bpm_}, {QStringLiteral("drawMs"), drawMs_},
                {QStringLiteral("audioConnected"), socket_.state() == QLocalSocket::ConnectedState},
                {QStringLiteral("audioError"), audioError_}, {QStringLiteral("stateError"), stateError_},
                {QStringLiteral("width"), width()}, {QStringLiteral("height"), height()},
                {QStringLiteral("parentWidth"), cube_ ? cube_->width() : 0},
                {QStringLiteral("fpsLimit"), 30}, {QStringLiteral("particleLimit"), 24},
                {QStringLiteral("theme"), theme_}, {QStringLiteral("songKey"), songKey_},
                {QStringLiteral("themeSource"), themeSource_}, {QStringLiteral("automaticTheme"), automaticTheme_},
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
        if (currentAct() == QStringLiteral("kolo")) drawKolo(p, w, h, u, t);
        else if (currentAct() == QStringLiteral("tram")) drawTram(p, w, h, u, t);
        else if (currentAct() == QStringLiteral("stadium")) drawStadium(p, w, h, u, t);
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
        return act_ == QStringLiteral("auto") ? danceActs().at(cycle_) : act_;
    }
    void saveFormCycle()
    {
        cube_->window()->setProperty("partyDanceCycle", cycle_);
        cube_->window()->setProperty("partyDanceFormStartedAt", formStartedAt_);
        cube_->window()->setProperty("partyDanceFormIntervalMs", formIntervalMs_);
    }
    void rotateForms()
    {
        if (act_ != QStringLiteral("auto")) return;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now < formStartedAt_) { formStartedAt_ = now; saveFormCycle(); return; }
        const qint64 elapsed = (now - formStartedAt_) / formIntervalMs_;
        if (!elapsed) return;
        cycle_ = (cycle_ + elapsed % danceActs().size()) % danceActs().size();
        formStartedAt_ += elapsed * formIntervalMs_;
        saveFormCycle(); update();
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
        if (!bytes_.isEmpty() && bytes == bytes_) { stateError_.clear(); return; }
        QJsonParseError error; const auto document = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) { stateError_ = QStringLiteral("Invalid party state"); return; }
        bytes_ = bytes; stateError_.clear();
        const auto scene = document.object().value(QStringLiteral("scene")).toObject();
        energy_ = std::clamp(scene.value(QStringLiteral("energy")).toDouble(.7), 0.0, 1.0);
        cool_ = scene.value(QStringLiteral("palette")).toString() == QStringLiteral("ice");
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
            if (words.contains(QStringLiteral("koktel"))) automaticTheme_ = QStringLiteral("cocktail");
            else if (words.contains(QStringLiteral("piti")) || words.contains(QStringLiteral("pivo")) || words.contains(QStringLiteral("vino"))) automaticTheme_ = QStringLiteral("bottles");
            else if (words.contains(QStringLiteral("gdje")) && words.contains(QStringLiteral("bio"))) automaticTheme_ = QStringLiteral("compass");
            else if (words.contains(QStringLiteral("masla"))) automaticTheme_ = QStringLiteral("dandelion");
            else if (words.contains(QStringLiteral("pepel"))) automaticTheme_ = QStringLiteral("embers");
            else {
                const QString defaults[] = {QStringLiteral("record"), QStringLiteral("speaker"), QStringLiteral("headphones"), QStringLiteral("microphone")};
                automaticTheme_ = defaults[songRevision_ % 4];
            }
        }
        theme_ = automaticTheme_; themeSource_ = QStringLiteral("automatic");
        const auto cue = scene.value(QStringLiteral("songVisual")).toObject();
        const QString uid = track.value(QStringLiteral("uid")).toString();
        const QString requested = cue.value(QStringLiteral("theme")).toString();
        if (!title.isEmpty() && !uid.isEmpty() && cue.value(QStringLiteral("trackUid")).toString() == uid && supportedTheme(requested)) {
            theme_ = requested; themeSource_ = QStringLiteral("cue");
        }
        cube_->window()->setProperty("partyDanceAutomaticTheme", automaticTheme_);
        cube_->window()->setProperty("partyDanceTheme", theme_);
        cube_->window()->setProperty("partyDanceThemeSource", themeSource_);
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
    void drawKolo(QPainter& p, double w, double h, double u, double t)
    {
        struct Dancer { QPointF foot, shoulder; double scale, step, bounce; int id; };
        std::array<Dancer, 8> dancers;
        std::array<QPointF, 8> hands;
        const QPointF center(w * .5, h * .65);
        const double rx = w * .32, ry = h * .13;
        p.setBrush(QColor(8, 20, 34, 70)); p.setPen(QPen(opacity(color(0), .26), 3 * u));
        p.drawEllipse(center + QPointF(0, 9 * u), rx + 35 * u, ry + 20 * u);
        for (int i = 0; i < 8; ++i) {
            const double angle = t * .15 + i * 2 * pi / 8, depth = std::sin(angle);
            const double step = std::sin(t * 3.6 + i * .12), scale = u * (1.40 + .20 * depth);
            const QPointF foot = center + QPointF(rx * std::cos(angle), ry * depth);
            const double bounce = (5 * std::abs(step) + 2 * pulse_) * scale;
            dancers[i] = {foot, foot - QPointF(0, 73 * scale + bounce), scale, step, bounce, i};
        }
        for (int i = 0; i < 8; ++i)
            hands[i] = (dancers[i].shoulder + dancers[(i + 1) % 8].shoulder) * .5 + QPointF(0, -12 * u * std::sin(t * 1.8));
        // Both neighbouring arms share the same hand point, closing the circle.
        for (int i = 0; i < 8; ++i) {
            const auto& d = dancers[i];
            p.setPen(QPen(opacity(color(i), .95), 6 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            for (const auto& hand : {hands[(i + 7) % 8], hands[i]}) {
                const double side = hand.x() < d.shoulder.x() ? -1 : 1;
                const QPointF shoulder = d.shoulder + QPointF(side * 23 * d.scale, 0);
                p.drawPolyline(QPolygonF{shoulder, (shoulder + hand) * .5 + QPointF(0, 8 * u), hand});
            }
            p.setBrush(QColor("#fff1b8")); p.drawEllipse(hands[i], 5 * u, 5 * u);
        }
        std::sort(dancers.begin(), dancers.end(), [](const Dancer& a, const Dancer& b) { return a.foot.y() < b.foot.y(); });
        for (const auto& d : dancers) {
            const QColor accent = color(d.id);
            p.save(); p.translate(d.foot); p.scale(d.scale, d.scale);
            const double bounce = d.bounce / d.scale;
            p.setPen(Qt::NoPen); p.setBrush(QColor(0, 0, 0, 65)); p.drawEllipse(QRectF(-36, -2, 72, 12));
            p.setPen(QPen(accent, 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            for (int side : {-1, 1}) {
                const double step = side * d.step;
                const QPointF foot(side * 27 + d.step * 7, -std::max(0.0, step) * 10);
                p.drawPolyline(QPolygonF{QPointF(side * 13, -34 - bounce), QPointF(side * 21, -17 - step * 5), foot});
                p.drawLine(foot - QPointF(9, 0), foot + QPointF(9, 0));
            }
            p.translate(0, -bounce); p.setBrush(QColor("#0b2033")); p.setPen(QPen(accent, 3.5));
            p.drawRoundedRect(QRectF(-27, -83, 54, 49), 8, 8);
            p.drawRoundedRect(QRectF(-34, -128, 68, 37), 10, 10);
            p.drawLine(QPointF(0, -128), QPointF(0, -143));
            p.setBrush(accent); p.drawEllipse(QPointF(0, -146), 5, 5);
            p.setBrush(opacity(accent, .45)); p.drawEllipse(QPointF(0, -59), 15 + pulse_, 15 + pulse_);
            p.setPen(Qt::NoPen); p.setBrush(QColor("#edffff"));
            p.drawEllipse(QPointF(-12, -112), 5, 6); p.drawEllipse(QPointF(12, -112), 5, 6);
            p.setPen(QPen(QColor("#edffff"), 3, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(QRectF(-10, -111, 20, 16), 205 * 16, 130 * 16);
            p.restore();
        }
    }
    void drawTram(QPainter& p, double w, double h, double u, double t)
    {
        // A travelling city backdrop and one large blue tram, with dancing passengers.
        for (int building = 0; building < 9; ++building) {
            const double x = w * (fraction(building / 9.0 - t * .011) * 1.2 - .1);
            const double height = (125 + (building * 53) % 110) * u, bottom = h * .69;
            p.setPen(QPen(opacity(color(building), .35), 2 * u));
            p.setBrush(QColor(15, 34 + building % 3 * 7, 56, 210));
            p.drawRoundedRect(QRectF(x, bottom - height, w * .12, height), 5 * u, 5 * u);
            for (int row = 0; row < 4; ++row) for (int col = 0; col < 3; ++col) {
                p.setPen(Qt::NoPen); p.setBrush(opacity(color(building + row), .28 + .15 * pulse_));
                p.drawRect(QRectF(x + (15 + col * 27) * u, bottom - height + (20 + row * 35) * u, 13 * u, 20 * u));
            }
        }
        const double rail = h * .79;
        p.setPen(QPen(QColor("#92b9ca"), 3 * u)); p.drawLine(QPointF(0, rail), QPointF(w, rail));
        p.drawLine(QPointF(0, rail + 17 * u), QPointF(w, rail + 17 * u));
        for (int tie = 0; tie < 28; ++tie) {
            const double x = w * fraction(tie / 28.0 - t * .04);
            p.setPen(QPen(QColor(72, 93, 108, 150), 4 * u)); p.drawLine(QPointF(x, rail), QPointF(x - 8 * u, rail + 17 * u));
        }
        const double tw = w * .79, th = 245 * u;
        const double x = w * .5 - tw * .5 + w * .065 * std::sin(t * .17);
        const double y = rail - th - 24 * u + 2 * u * std::sin(t * 4);
        p.setPen(QPen(QColor("#c5e0ea"), 3 * u));
        p.drawLine(QPointF(0, y - 65 * u), QPointF(w, y - 65 * u));
        const double pole = x + tw * .55;
        p.drawPolyline(QPolygonF{QPointF(pole - 35 * u, y), QPointF(pole + 15 * u, y - 34 * u),
            QPointF(pole - 20 * u, y - 65 * u), QPointF(pole + 35 * u, y - 65 * u)});
        for (double fraction : {.16, .25, .72, .81}) {
            const QPointF axle(x + tw * fraction, y + th + 10 * u);
            p.setPen(QPen(QColor("#c8e6f3"), 3 * u)); p.setBrush(QColor("#081320")); p.drawEllipse(axle, 25 * u, 25 * u);
            p.drawLine(axle - QPointF(std::cos(t * 4) * 18 * u, std::sin(t * 4) * 18 * u),
                       axle + QPointF(std::cos(t * 4) * 18 * u, std::sin(t * 4) * 18 * u));
        }
        QLinearGradient body(0, y, 0, y + th); body.setColorAt(0, QColor("#55bfff")); body.setColorAt(1, QColor("#0752b6"));
        p.setPen(QPen(QColor("#bceeff"), 3 * u)); p.setBrush(body); p.drawRoundedRect(QRectF(x, y, tw, th), 25 * u, 25 * u);
        const double windowGap = 12 * u, windowWidth = (tw - 38 * u - 5 * windowGap) / 6;
        for (int passenger = 0; passenger < 6; ++passenger) {
            const QRectF window(x + 19 * u + passenger * (windowWidth + windowGap), y + 43 * u, windowWidth, 147 * u);
            p.setPen(QPen(QColor("#c6edff"), 3 * u)); p.setBrush(QColor("#0b243d")); p.drawRoundedRect(window, 10 * u, 10 * u);
            p.save(); p.setClipRect(window.adjusted(2 * u, 2 * u, -2 * u, -2 * u));
            drawFan(p, window.center().x(), window.bottom() + 25 * u, u * 1.13,
                    .6 + .35 * std::sin(t * 2.6 + passenger * .8), passenger, false, 1, t);
            p.restore();
        }
        p.setPen(Qt::NoPen); p.setBrush(QColor("#07263e"));
        p.drawRoundedRect(QRectF(x + 20 * u, y + 9 * u, tw - 40 * u, 26 * u), 5 * u, 5 * u);
        QFont font(QStringLiteral("DejaVu Sans")); font.setPixelSize(qRound(18 * u)); font.setBold(true); p.setFont(font);
        p.setPen(QColor("#ffe59b")); p.drawText(QRectF(x + 30 * u, y + 9 * u, tw - 60 * u, 26 * u),
                                             Qt::AlignCenter, QStringLiteral("30  LARICS  ·  SLJEDEĆA: REFREN"));
        p.setPen(QPen(QColor("#e1f7ff"), 3 * u)); p.drawLine(QPointF(x + 16 * u, y + 207 * u), QPointF(x + tw - 16 * u, y + 207 * u));
        p.setPen(Qt::NoPen); p.setBrush(QColor("#ffe9aa"));
        p.drawRoundedRect(QRectF(x + tw - 38 * u, y + 215 * u, 24 * u, 12 * u), 4 * u, 4 * u);
        p.setBrush(QColor("#ff9275")); p.drawRoundedRect(QRectF(x + 14 * u, y + 215 * u, 15 * u, 12 * u), 4 * u, 4 * u);
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
    QByteArray buffer_, bytes_; QString stateFile_, socketPath_, audioError_, stateError_, act_, songKey_;
    QString theme_ = QStringLiteral("record"), automaticTheme_ = QStringLiteral("record"), themeSource_ = QStringLiteral("automatic");
    quint64 frames_ = 0, beats_ = 0; qint64 formStartedAt_ = 0, formIntervalMs_ = 60000;
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
        if (act != QStringLiteral("auto") && !danceActs().contains(act)) {
            host->fail(host->invocation_context, "act must be auto, disco, streamers, conga, stadium, kolo or tram"); return;
        }
    }
    if (request.contains(QStringLiteral("startAct")) && !danceActs().contains(request.value(QStringLiteral("startAct")).toString())) {
        host->fail(host->invocation_context, "startAct must name a supported dance act"); return;
    }
    if (request.contains(QStringLiteral("formSeconds"))) {
        const auto value = request.value(QStringLiteral("formSeconds")); const double seconds = value.toDouble();
        if (!value.isDouble() || !std::isfinite(seconds) || seconds < 1 || seconds > 600 || seconds != std::floor(seconds)) {
            host->fail(host->invocation_context, "formSeconds must be a whole number from 1 to 600"); return;
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
