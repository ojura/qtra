// A party display that follows tools/party_dj's JSON file while qtra keeps
// rendering and analysing the system audio. The fleet's colours and silhouettes
// follow the LARICS Spomenar robots; their cargo tonight is a record.
#include "agent/agent_abi.h"
#include "cube_widget.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCache>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEasingCurve>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QHash>
#include <QHelpEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QImageReader>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenuBar>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPointer>
#include <QProgressBar>
#include <QProcess>
#include <QPropertyAnimation>
#include <QQueue>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextLayout>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>

namespace {
constexpr double pi = 3.14159265358979323846;
double unit(double x) { return std::clamp(x, 0.0, 1.0); }
double smooth(double x) { x = unit(x); return x * x * (3.0 - 2.0 * x); }
double number(const QJsonObject& o, const char* key, double fallback)
{
    const double value = o.value(QLatin1String(key)).toDouble(fallback);
    return std::isfinite(value) ? value : fallback;
}
QString text(const QJsonObject& o, const char* key, const QString& fallback = {})
{
    return o.value(QLatin1String(key)).toString(fallback);
}
QColor alpha(QColor c, double opacity) { c.setAlphaF(unit(opacity)); return c; }
QFont stageFont(double pixels, bool bold = false)
{
    QFont result(QStringLiteral("DejaVu Sans"));
    result.setPixelSize(std::max(10, qRound(pixels)));
    result.setWeight(bold ? QFont::DemiBold : QFont::Normal);
    return result;
}
// One readable native text scale for controls and painted metadata. The stage
// reference matches the free pane beside the 600px desk on the target laptop.
struct UiMetrics {
    static constexpr int body = 29, secondary = 26, small = 24, icon = 34;
    static constexpr double reference = 1.5;
};
QFont uiFont(double scale, int pixels = UiMetrics::body, bool bold = false)
{
    return stageFont(pixels * scale / UiMetrics::reference, bold);
}
enum class GuestShortcut : std::size_t { Spotify, Playback, Search, Previous, Next, Lyrics, Count };
struct ShortcutSpec { const char* objectName; const char* key; const char* label; const char* alias; };
constexpr std::array<ShortcutSpec, static_cast<std::size_t>(GuestShortcut::Count)> guestShortcuts{{
    {"partySpotifyAction", "Tab", "Spotify", ""},
    {"partyPlaybackAction", "Ctrl+P", "Pauza", ""},
    {"partyRequestAction", "Ctrl+K", "Traži", "R"},
    {"partyPreviousAction", "Ctrl+B", "Natrag", ""},
    {"partyNextAction", "Ctrl+N", "Dalje", ""},
    {"partyLyricsAction", "Ctrl+L", "Stihovi", ""}
}};
const ShortcutSpec& shortcutSpec(GuestShortcut kind) { return guestShortcuts[static_cast<std::size_t>(kind)]; }
QString shortcutKey(GuestShortcut kind)
{
    return QKeySequence(QString::fromUtf8(shortcutSpec(kind).key)).toString(QKeySequence::NativeText);
}
QString controlStyle(double scale, int pixels = UiMetrics::body)
{
    return QStringLiteral("QPushButton { font:600 %1px 'DejaVu Sans'; color:#ddfff4; background:#1b4a44; border:1px solid #6bc6ab; border-radius:%2px; padding:%3px; } QPushButton:disabled { color:#aab5c5; background:#263141; border-color:#536174; } QPushButton:hover:enabled { background:#276258; } QPushButton:checked { color:#fff1d9; background:#654a29; border-color:#ffd089; }")
        .arg(uiFont(scale, pixels).pixelSize()).arg(std::max(3, qRound(6 * scale))).arg(std::max(1, qRound(2 * scale)));
}
double requestCardWidth(double width, double scale)
{
    const double infoWidth = std::min(width * .53, 690 * scale);
    return std::min(400 * scale, std::max(120 * scale, width - infoWidth - 102 * scale));
}
QString coverUrl(const QJsonObject& track)
{
    QString wanted = text(track, "artUrl");
    if (wanted.isEmpty()) wanted = text(track, "image");
    if (wanted.isEmpty()) wanted = text(track.value(QStringLiteral("image")).toObject(), "url");
    const QUrl url(wanted, QUrl::StrictMode);
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() && wanted.size() <= 2048
        && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https")) ? wanted : QString();
}
QImage decodeCover(QByteArray& bytes, int side, QString& error)
{
    if (bytes.size() > 2 * 1024 * 1024) { error = QStringLiteral("Cover exceeds 2 MiB"); return {}; }
    QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer);
    const QSize size = reader.size(); const QByteArray format = reader.format().toLower();
    if (!size.isValid() || size.width() > 4096 || size.height() > 4096 || (format != "jpeg" && format != "png" && format != "webp")) {
        error = QStringLiteral("Unsupported cover size or format"); return {};
    }
    reader.setScaledSize(size.scaled(side, side, Qt::KeepAspectRatio)); reader.setAutoTransform(true);
    QImage image = reader.read(); if (image.isNull()) error = reader.errorString();
    return image;
}
QCursor djCursor(Qt::CursorShape shape = Qt::ArrowCursor)
{
    QPixmap image(48, 48); image.fill(Qt::transparent);
    QPainter p(&image); p.setRenderHint(QPainter::Antialiasing);
    if (shape == Qt::IBeamCursor) {
        QPainterPath beam; beam.moveTo(24, 5); beam.lineTo(24, 43);
        beam.moveTo(16, 5); beam.lineTo(32, 5); beam.moveTo(16, 43); beam.lineTo(32, 43);
        p.setPen(QPen(QColor("#07111e"), 7, Qt::SolidLine, Qt::RoundCap)); p.drawPath(beam);
        p.setPen(QPen(QColor("#f1fbff"), 3, Qt::SolidLine, Qt::RoundCap)); p.drawPath(beam);
        p.end(); return QCursor(image, 24, 24);
    }
    QPainterPath arrow; arrow.moveTo(7, 4); arrow.lineTo(7, 36); arrow.lineTo(16, 28);
    arrow.lineTo(23, 44); arrow.lineTo(30, 40); arrow.lineTo(23, 25); arrow.lineTo(37, 25); arrow.closeSubpath();
    p.setPen(QPen(QColor("#07111e"), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.setBrush(QColor("#f1fbff")); p.drawPath(arrow);
    p.end(); return QCursor(image, 7, 4);
}
void panel(QPainter& p, const QRectF& r, const QColor& stroke, double opacity = 0.84)
{
    p.setPen(QPen(alpha(stroke, 0.30), 1));
    p.setBrush(QColor(11, 15, 29, qRound(255 * opacity)));
    p.drawRoundedRect(r, 16, 16);
}
void line(QPainter& p, QPointF a, QPointF b, QColor c, double width = 2)
{
    p.setPen(QPen(c, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(a, b);
}
void disc(QPainter& p, QPointF at, double radius, QColor colour, double turn)
{
    p.save();
    p.translate(at); p.rotate(turn);
    p.setPen(QPen(QColor("#46536c"), 1));
    p.setBrush(QColor("#101522")); p.drawEllipse(QPointF(), radius, radius);
    p.setBrush(Qt::NoBrush);
    for (int i = 1; i <= 3; ++i) {
        p.setPen(QPen(QColor(96, 114, 141, 90), 0.8));
        p.drawEllipse(QPointF(), radius * (0.43 + i * 0.13), radius * (0.43 + i * 0.13));
    }
    p.setPen(Qt::NoPen); p.setBrush(colour);
    p.drawEllipse(QPointF(), radius * 0.32, radius * 0.32);
    p.setBrush(QColor("#141a2a")); p.drawEllipse(QPointF(), 2.2, 2.2);
    line(p, QPointF(radius * .4, -radius * .3), QPointF(radius * .74, -radius * .48),
         QColor(220, 237, 255, 110), 2);
    p.restore();
}

// Vehicle coordinates are independent of the view. Text is drawn after the
// transform is restored, so labels keep their readable screen size.
void robot(QPainter& p, int kind, QPointF at, double scale, double time,
           double travelled, double direction, double pulse, bool carrying)
{
    const bool air = kind == 0 || kind == 3;
    const QColor ink("#0a1728"), steel("#647b95"), light("#e6f4ff");
    const QColor colour = kind == 0 ? QColor("#d9534f") : kind == 1 ? QColor("#f17c75")
        : kind == 2 ? QColor("#729bef") : QColor("#ffbd70");
    p.save(); p.translate(at); p.scale(scale, scale);
    const double bob = air ? std::sin(time * 2.4 + kind) * 2.5 : -pulse * 2.0;
    p.translate(0, bob);
    if (air) p.rotate(std::sin(time * .9 + kind) * 3.0 + direction * 3.0);
    const auto box = [&](QRectF r, QColor fill, double radius = 4) {
        p.setPen(QPen(ink, 1.3)); p.setBrush(fill); p.drawRoundedRect(r, radius, radius);
    };
    if (air) {
        const double span = kind == 3 ? 43.0 : 31.0;
        for (double sign : {-1.0, 1.0}) {
            for (double depth : {-1.0, 1.0}) {
                const QPointF motor(sign * span, depth * 11);
                line(p, QPointF(), motor, ink, 6);
                line(p, QPointF(0, -1), motor - QPointF(0, 1), steel, 3);
                p.setPen(QPen(light, 1)); p.setBrush(QColor("#33485f"));
                p.drawEllipse(motor, 5, 3);
                p.setPen(QPen(alpha(colour, .7), 2));
                const double rotor = 12 + 3 * std::sin(time * 42 + sign + depth);
                p.drawEllipse(motor - QPointF(0, 3), rotor, 2.2);
                line(p, motor - QPointF(rotor, 3), motor + QPointF(rotor, -3), light, 1);
            }
        }
        box(QRectF(-14, -15, 28, 29), colour, 9);
        box(QRectF(-10, -10, 20, 12), ink);
        for (double sign : {-1.0, 1.0}) {
            line(p, QPointF(sign * 8, 9), QPointF(sign * 15, 24), steel, 2);
            line(p, QPointF(sign * 15, 24), QPointF(sign * 23, 24), steel, 2);
        }
        if (carrying) {
            const double swing = std::sin(time * 2.1) * 7.0;
            line(p, QPointF(-7, 14), QPointF(swing, 43), light, 1.3);
            line(p, QPointF(7, 14), QPointF(swing, 43), light, 1.3);
            disc(p, QPointF(swing, 54), 15, QColor("#ff843b"), time * 30);
        }
    } else {
        for (double x : {-20.0, 0.0, 20.0}) {
            p.setPen(QPen(steel, 1.5)); p.setBrush(ink); p.drawEllipse(QPointF(x, 15), 8, 8);
            const double a = travelled / 7;
            line(p, QPointF(x, 15), QPointF(x + std::cos(a) * 5, 15 + std::sin(a) * 5), light, 1);
        }
        box(QRectF(-28, -7, 55, 22), colour, 5);
        box(QRectF(-18, -23, 36, 19), QColor("#9babb6"), 4);
        box(QRectF(-11, -18, 23, 11), ink, 3);
        if (kind == 2) {
            for (double x : {24.0, 32.0}) line(p, QPointF(x, -35), QPointF(x, 18), light, 3);
            line(p, QPointF(24, -35), QPointF(32, -35), light, 3);
            line(p, QPointF(27, 4), QPointF(51, 4), light, 3);
            if (carrying) disc(p, QPointF(47, -12), 16, QColor("#ff843b"), time * 30);
        } else {
            line(p, QPointF(-21, -22), QPointF(-21, -36), steel, 2);
            p.setPen(Qt::NoPen); p.setBrush(light); p.drawEllipse(QPointF(-21, -36), 4, 2);
            if (carrying) disc(p, QPointF(0, -39), 16, QColor("#ff843b"), time * 30);
        }
        // A two-link waving arm. Its endpoint follows the beat without moving
        // the body or its cargo, the same separation used by the book's fleet.
        const QPointF shoulder(-22, -4), hand(-39, -26 - pulse * 15);
        const QPointF elbow(-37 + std::sin(time * 3) * 3, -7);
        line(p, shoulder, elbow, steel, 3); line(p, elbow, hand, light, 2);
    }
    const double blink = std::fmod(time + kind * 1.3, 5.7) < .13 ? .6 : 2.4;
    p.setPen(Qt::NoPen); p.setBrush(QColor("#9af6ff"));
    p.drawEllipse(QPointF(-4 + direction * 1.1, air ? -5 : -13), 2, blink);
    p.drawEllipse(QPointF(4 + direction * 1.1, air ? -5 : -13), 2, blink);
    p.restore();
    p.setFont(stageFont(std::clamp(scale * 10, 14.0, 32.0), true));
    p.setPen(alpha(light, .82));
    const QString name = kind == 0 ? QStringLiteral("EagleRed")
        : kind == 1 ? QStringLiteral("VIV") : kind == 2 ? QStringLiteral("Navines") : QStringLiteral("MORUS");
    const QFontMetricsF label(p.font());
    const double labelWidth = label.horizontalAdvance(name) + 24;
    p.drawText(QRectF(at.x() - labelWidth / 2, at.y() + scale * (carrying && air ? 76 : 34), labelWidth, label.height() + 4),
               Qt::AlignCenter, name);
}

struct Cue {
    QString id, text;
    double seconds = 7;
    qint64 createdAt = 0;
};

class RequestDesk final : public QDockWidget {
public:
    explicit RequestDesk(QMainWindow* window, const QString& baseUrl)
        : QDockWidget(QStringLiteral("DJ · pjesme i razgovor"), window), base_(baseUrl)
    {
        setObjectName(QStringLiteral("partyRequestPanel"));
        setAllowedAreas(Qt::RightDockWidgetArea);
        setFeatures(QDockWidget::NoDockWidgetFeatures);
        setFixedWidth(600);
        setStyleSheet(QStringLiteral(
            "QDockWidget,QLabel,QLineEdit,QPlainTextEdit,QListWidget,QTextBrowser,QPushButton { font:%1px 'DejaVu Sans'; }"
            "QDockWidget { color:#f7f2ea; background:#101727; }"
            "QDockWidget::title { background:#202a3c; padding:15px; }"
            "QWidget#partyDeskBody { background:#101727; }"
            "QWidget#partySuggestions { background:#183e56; border:2px solid #85e8f2; border-radius:10px; }"
            "QLabel { color:#c1cce0; }"
            "QLineEdit,QPlainTextEdit,QListWidget,QTextBrowser,QLabel#partySuggestionHint,QWidget#partySelectedSong { background:#080f1d; color:#f7f2ea; border:1px solid #344158; border-radius:10px; padding:11px; }"
            "QListWidget#partyResults,QLabel#partySuggestionHint,QWidget#partySelectedSong { background:#183e56; color:#f3fcff; border:0; }"
            "QLineEdit:focus,QPlainTextEdit:focus { border-color:#7de4ed; }"
            "QListWidget::item { padding:10px 4px; border-bottom:1px solid #49758c; }"
            "QListWidget::item:hover { background:#2c617a; color:#ffffff; }"
            "QListWidget::item:selected { background:#a2ebf0; color:#092536; border-left:5px solid #ffbc75; }"
            "QScrollBar:vertical { background:#13243a; border:0; }"
            "QScrollBar::handle:vertical { background:#9bb5d1; border-radius:4px; }"
            "QScrollBar::handle:vertical:hover { background:#d4efff; }"
            "QPushButton { background:#ff8a42; color:#121725; border:0; border-radius:10px; padding:14px; font-weight:600; }"
            "QPushButton#partyDescribeButton,QPushButton#partyDraftButton { background:#234250; color:#ddf8ff; font-size:%2px; }"
            "QPushButton:disabled { background:#364258; color:#9ba9be; }"
            "QPushButton#partyDescribeButton:disabled,QPushButton#partyDraftButton:disabled { background:#263545; color:#91a0b5; }"
            "QPushButton:hover { background:#ffad72; }").arg(UiMetrics::body).arg(UiMetrics::secondary));
        auto* titleBar = new QWidget(this); titleBar->setObjectName(QStringLiteral("partyDeskTitleBar"));
        titleBar->setStyleSheet(QStringLiteral("QWidget#partyDeskTitleBar { background:#202a3c; }"));
        auto* titleLayout = new QHBoxLayout(titleBar); titleLayout->setContentsMargins(21, 13, 21, 13);
        auto* title = new QLabel(QStringLiteral("DJ · razgovor"), titleBar); titleLayout->addWidget(title, 1);
        onlineBadge_ = new QLabel(titleBar); onlineBadge_->setObjectName(QStringLiteral("partyDjOnlineBadge"));
        onlineBadge_->setAlignment(Qt::AlignRight | Qt::AlignVCenter); titleLayout->addWidget(onlineBadge_);
        setTitleBarWidget(titleBar); presenceClock_.start(); updateDjPresence();
        auto* body = new QWidget(this); body->setObjectName(QStringLiteral("partyDeskBody"));
        auto* layout = new QVBoxLayout(body); layout->setContentsMargins(21, 19, 21, 10); layout->setSpacing(11);
        name_ = new QLineEdit(body); name_->setObjectName(QStringLiteral("partyName"));
        name_->setPlaceholderText(QStringLiteral("Tvoje ime (po želji)")); name_->setMaxLength(48);
        QSettings settings(QStringLiteral("qtra"), QStringLiteral("party"));
        guestId_ = settings.value(QStringLiteral("guestId")).toString();
        if (guestId_.isEmpty()) { guestId_ = uuid(); settings.setValue(QStringLiteral("guestId"), guestId_); }
        composerId_ = uuid();
        name_->setText(settings.value(QStringLiteral("name"), QStringLiteral("Laptop")).toString());
        connect(name_, &QLineEdit::editingFinished, this, [this] {
            QSettings(QStringLiteral("qtra"), QStringLiteral("party")).setValue(QStringLiteral("name"), name_->text().trimmed());
        });
        layout->addWidget(name_);
        presence_ = new QLabel(QStringLiteral("Razgovor s DJ-em"), body); presence_->setWordWrap(true); layout->addWidget(presence_);
        presence_->setObjectName(QStringLiteral("partyDjPresence"));
        presence_->setFixedHeight(73); presence_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        chat_ = new QTextBrowser(body); chat_->setObjectName(QStringLiteral("partyChat"));
        chat_->setOpenLinks(false);
        connect(chat_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
            if (url.scheme() == QStringLiteral("recall") && url.path().toLongLong() == lastSentId_) recallMessage();
        });
        chat_->setOpenExternalLinks(false); chat_->setMinimumHeight(175); layout->addWidget(chat_, 1);
        chat_->verticalScrollBar()->setObjectName(QStringLiteral("partyChatScroll"));
        chatScroll_ = new QPropertyAnimation(chat_->verticalScrollBar(), QByteArrayLiteral("value"), this);
        chatScroll_->setObjectName(QStringLiteral("partyChatScrollAnimation"));
        chatScroll_->setEasingCurve(QEasingCurve::InOutCubic);
        connect(chat_->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) {
            // QTextBrowser can finish laying out new paragraphs after setHtml.
            // Keep following that later extent until the reader interacts.
            if (followingBottom_) animateChatToBottom(220);
        });
        suggestionArea_ = new QWidget(body); auto* suggestionArea = suggestionArea_;
        suggestionArea->setObjectName(QStringLiteral("partySuggestions"));
        suggestionArea->setFixedHeight(340); suggestionArea->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        auto* suggestionLayout = new QVBoxLayout(suggestionArea); suggestionLayout->setContentsMargins(3, 3, 3, 3); suggestionLayout->setSpacing(0);
        searchActivity_ = new QProgressBar(suggestionArea); searchActivity_->setObjectName(QStringLiteral("partySearchActivity"));
        searchActivity_->setTextVisible(false); searchActivity_->setFixedHeight(5); searchActivity_->setRange(0, 1); searchActivity_->setValue(0);
        searchActivity_->setStyleSheet(QStringLiteral("QProgressBar { background:#162235; border:0; } QProgressBar::chunk { background:#7de4ed; }"));
        suggestionLayout->addWidget(searchActivity_);
        suggestions_ = new QStackedWidget(suggestionArea); suggestions_->setObjectName(QStringLiteral("partySuggestionPages"));
        suggestions_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding); suggestionLayout->addWidget(suggestions_, 1);
        results_ = new QListWidget(suggestions_); results_->setObjectName(QStringLiteral("partyResults"));
        results_->setIconSize(QSize(72, 72));
        results_->setWordWrap(false); results_->setTextElideMode(Qt::ElideRight);
        results_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        results_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        results_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel); results_->verticalScrollBar()->setSingleStep(23);
        results_->setMouseTracking(true);
        suggestions_->addWidget(results_);
        suggestionHint_ = new QLabel(suggestions_); suggestionHint_->setObjectName(QStringLiteral("partySuggestionHint"));
        suggestionHint_->setWordWrap(true); suggestionHint_->setTextFormat(Qt::PlainText);
        suggestionHint_->setAlignment(Qt::AlignCenter); suggestionHint_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        suggestions_->addWidget(suggestionHint_);
        selectedPage_ = new QWidget(suggestions_); selectedPage_->setObjectName(QStringLiteral("partySelectedSong"));
        auto* selectedLayout = new QVBoxLayout(selectedPage_); selectedLayout->setContentsMargins(15, 13, 15, 13); selectedLayout->setSpacing(6);
        selectedTitle_ = new QLabel(selectedPage_); selectedTitle_->setObjectName(QStringLiteral("partySelectedTitle"));
        selectedTitle_->setTextFormat(Qt::PlainText); selectedTitle_->setWordWrap(true); selectedTitle_->setFixedHeight(73);
        selectedTitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); selectedLayout->addWidget(selectedTitle_);
        selectedArtist_ = new QLabel(selectedPage_); selectedArtist_->setObjectName(QStringLiteral("partySelectedArtist"));
        selectedArtist_->setTextFormat(Qt::PlainText); selectedArtist_->setFixedHeight(36);
        selectedArtist_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); selectedLayout->addWidget(selectedArtist_);
        selectedLayout->addStretch(1);
        cancelSong_ = new QPushButton(QStringLiteral("Odustani od pjesme"), selectedPage_);
        cancelSong_->setObjectName(QStringLiteral("partyCancelSong")); selectedLayout->addWidget(cancelSong_);
        suggestions_->addWidget(selectedPage_);
        message_ = new QPlainTextEdit(body); message_->setObjectName(QStringLiteral("partyMessage"));
        message_->setPlaceholderText(QStringLiteral("Magic box: pjesma, izvođač ili poruka DJ-u…"));
        message_->setLineWrapMode(QPlainTextEdit::WidgetWidth); message_->setTabChangesFocus(true);
        message_->installEventFilter(this);
        message_->setFixedHeight(178); layout->addWidget(message_);
        auto* actions = new QWidget(body); actions->setObjectName(QStringLiteral("partyMessageActions")); actions->setFixedHeight(62);
        auto* actionLayout = new QHBoxLayout(actions); actionLayout->setContentsMargins(0, 0, 0, 0); actionLayout->setSpacing(11);
        describe_ = new QPushButton(QStringLiteral("Traži po opisu"), actions); describe_->setObjectName(QStringLiteral("partyDescribeButton"));
        alternate_ = new QPushButton(QStringLiteral("Vrati skicu"), actions); alternate_->setObjectName(QStringLiteral("partyDraftButton"));
        for (auto* button : {describe_, alternate_}) { button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding); actionLayout->addWidget(button, 1); }
        layout->addWidget(actions);
        magicHelp_ = new QLabel(body); magicHelp_->setObjectName(QStringLiteral("partyMagicHelp"));
        magicHelp_->setWordWrap(true); magicHelp_->setTextFormat(Qt::RichText); magicHelp_->setFixedHeight(73);
        magicHelp_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); layout->addWidget(magicHelp_);
        send_ = new QPushButton(QStringLiteral("Pošalji poruku (Enter)"), body); send_->setObjectName(QStringLiteral("partySendButton")); layout->addWidget(send_);
        searchDelay_.setSingleShot(true); searchDelay_.setInterval(330);
        connect(message_, &QPlainTextEdit::textChanged, this, [this] { composerChanged(); });
        connect(&searchDelay_, &QTimer::timeout, this, [this] { search(); });
        connect(results_, &QListWidget::itemSelectionChanged, this, [this] { chooseSong(); });
        connect(cancelSong_, &QPushButton::clicked, this, [this] { cancelSong(); });
        connect(send_, &QPushButton::clicked, this, [this] { sendMagic(); });
        connect(describe_, &QPushButton::clicked, this, [this] { beginDescription(); });
        connect(alternate_, &QPushButton::clicked, this, [this] { restoreAlternateDraft(); });
        descriptionPoll_.setInterval(1500);
        connect(&descriptionPoll_, &QTimer::timeout, this, [this] { pollDescription(); });
        notice_ = new QLabel(body); notice_->setObjectName(QStringLiteral("partyNotice"));
        notice_->setWordWrap(true); notice_->setTextFormat(Qt::PlainText); notice_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        notice_->setAttribute(Qt::WA_TransparentForMouseEvents);
        notice_->setStyleSheet(QStringLiteral("background:#1d3446; color:#e7f8ff; border:1px solid #6bafc6; border-radius:10px; padding:9px; font-size:%1px;").arg(UiMetrics::secondary));
        notice_->hide(); noticeDismiss_.setSingleShot(true); noticeDismiss_.setInterval(6000);
        connect(&noticeDismiss_, &QTimer::timeout, notice_, &QWidget::hide);
        for (QWidget* control : std::array<QWidget*, 13>{name_, message_, message_->viewport(), chat_, chat_->viewport(),
                                chat_->verticalScrollBar(), results_, results_->viewport(), results_->verticalScrollBar(), send_, cancelSong_, describe_, alternate_})
        {
            control->installEventFilter(this);
            control->setCursor(djCursor(control->cursor().shape()));
        }
        idleClock_.start(); idleReturn_.setInterval(250);
        connect(&idleReturn_, &QTimer::timeout, this, [this] {
            refreshWorkBadges(); updateDjPresence();
            if (isVisible() && idleClock_.elapsed() >= 120000 && QApplication::mouseButtons() == Qt::NoButton
                && !chat_->verticalScrollBar()->isSliderDown()) {
                animateChatToBottom(800); idleClock_.restart();
            }
        });
        idleReturn_.start();
        updateComposerMode(); composerChanged();
        setWidget(body);
        chatPoll_.setInterval(1500);
        connect(&chatPoll_, &QTimer::timeout, this, [this] { fetchChat(); });
        connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
            if (visible) {
                chatPoll_.start(); fetchChat(); message_->setFocus();
                QTimer::singleShot(0, this, [this] { updateSuggestionOverlay(); });
            }
            else chatPoll_.stop();
        });
        window->addDockWidget(Qt::RightDockWidgetArea, this); hide();
    }
    ~RequestDesk() override { cancelSearch(); cancelResultArtwork(); }
    void setBase(const QString& url)
    {
        const QUrl next(url);
        if (next == base_) return;
        invalidateSearchIntent(); ++serviceGeneration_; chatBusy_ = false;
        base_ = next; cursor_.clear(); messages_ = {}; messageIds_.clear(); processingIds_.clear(); messageWork_ = {}; workPresentation_ = {}; chatFetches_ = 0;
        presenceExpiryWatermark_ = 0; presenceWatermarkDeadline_ = 0;
        observeDjPresence({});
        withdrawnIds_.clear(); lastSentId_ = 0; recallBusy_ = false; alternateDraft_ = {}; cancelDescription(); ++searchVersion_;
        clearSearchResults();
        chatScroll_->stop(); followingBottom_ = false; chat_->clear();
        if (isVisible()) fetchChat();
    }
    QJsonObject status() const
    {
        int thumbnails = 0;
        for (int i = 0; i < results_->count(); ++i) if (results_->item(i)->data(Qt::UserRole + 1).toBool()) ++thumbnails;
        const auto rect = [](const QWidget* widget) {
            const QRect r = widget->geometry();
            return QJsonObject{{QStringLiteral("x"), r.x()}, {QStringLiteral("y"), r.y()},
                               {QStringLiteral("width"), r.width()}, {QStringLiteral("height"), r.height()}};
        };
        const QJsonObject bounds{{QStringLiteral("partyRequestPanel"), rect(this)},
            {QStringLiteral("partySuggestions"), rect(suggestionArea_)}, {QStringLiteral("partyChat"), rect(chat_)},
            {QStringLiteral("partyMessage"), rect(message_)}, {QStringLiteral("partyMagicHelp"), rect(magicHelp_)},
            {QStringLiteral("partySendButton"), rect(send_)}};
        return {{QStringLiteral("panelVisible"), isVisible()}, {QStringLiteral("chatCount"), messages_.size()},
                {QStringLiteral("panelBounds"), bounds},
                {QStringLiteral("selectedCancelFits"), selectedPage_->rect().contains(cancelSong_->geometry())},
                {QStringLiteral("processingCount"), activeWorkCount()}, {QStringLiteral("workStates"), workStates()},
                {QStringLiteral("lastSentMessageId"), lastSentId_}, {QStringLiteral("recallBusy"), recallBusy_},
                {QStringLiteral("alternateDraftAvailable"), !alternateDraft_.isEmpty()},
                {QStringLiteral("descriptionId"), descriptionId_}, {QStringLiteral("descriptionStatus"), descriptionStatus_},
                {QStringLiteral("searchArtwork"), QJsonObject{{QStringLiteral("loaded"), thumbnails},
                    {QStringLiteral("active"), resultArtReplies_.size()}, {QStringLiteral("cached"), resultArtCache_.size()}}},
                {QStringLiteral("searchIntent"), searchIntent()}, {QStringLiteral("automaticSearchIntent"), automaticSearchIntent()},
                {QStringLiteral("djPresence"), djPresence()},
                {QStringLiteral("chatQuietMs"), idleClock_.elapsed()}, {QStringLiteral("chatIdleReturnMs"), 120000},
                {QStringLiteral("composerCursorPixels"), message_->viewport()->cursor().pixmap().width()},
                {QStringLiteral("searchCount"), results_->count()}, {QStringLiteral("panelNotice"), notice_->text()},
                {QStringLiteral("panelNoticeVisible"), notice_->isVisible()},
                {QStringLiteral("panelBottomGap"), widget()->height() - send_->geometry().bottom() - 1},
                {QStringLiteral("composerMode"), selectedTrack_.isEmpty() ? QStringLiteral("message") : QStringLiteral("dedication")},
                {QStringLiteral("suggestionState"), suggestionState_}, {QStringLiteral("selectedTrack"), text(selectedTrack_, "title")}};
    }
    QJsonObject draft() const
    {
        auto result = editorDraft();
        result.insert(QStringLiteral("guestId"), guestId_);
        result.insert(QStringLiteral("composerId"), composerId_);
        result.insert(QStringLiteral("draftRevision"), static_cast<qint64>(draftRevision_));
        result.insert(QStringLiteral("editorMode"), selectedTrack_.isEmpty() ? QStringLiteral("message") : QStringLiteral("dedication"));
        result.insert(QStringLiteral("lastSentMessageId"), lastSentId_);
        result.insert(QStringLiteral("alternateDraft"), alternateDraft_);
        result.insert(QStringLiteral("alternateDraftLabel"), alternateLabel_);
        if (descriptionActive_) result.insert(QStringLiteral("description"),
            QJsonObject{{QStringLiteral("text"), descriptionText_}, {QStringLiteral("requestId"), descriptionRequestId_}});
        return result;
    }
    QJsonObject editorDraft() const
    {
        const auto cursor = message_->textCursor();
        return {{QStringLiteral("text"), message_->toPlainText()}, {QStringLiteral("cursor"), cursor.position()},
                {QStringLiteral("anchor"), cursor.anchor()}, {QStringLiteral("query"), savedQuery_},
                {QStringLiteral("queryCursor"), savedQueryCursor_}, {QStringLiteral("queryAnchor"), savedQueryAnchor_},
                {QStringLiteral("selectedTrack"), selectedTrack_}};
    }
    void showNotice(const QString& words) { setNotice(words); }
    void restoreDraft(const QJsonObject& draft)
    {
        if (draft.contains(QStringLiteral("lastSentMessageId"))) lastSentId_ = static_cast<qint64>(number(draft, "lastSentMessageId", 0));
        if (draft.contains(QStringLiteral("alternateDraft"))) {
            alternateDraft_ = draft.value(QStringLiteral("alternateDraft")).toObject();
            alternateLabel_ = text(draft, "alternateDraftLabel", QStringLiteral("Vrati skicu"));
        }
        applyEditorDraft(draft);
        const auto description = draft.value(QStringLiteral("description")).toObject();
        if (!description.isEmpty() && text(description, "text") == message_->toPlainText().trimmed() && selectedTrack_.isEmpty()) {
            descriptionRequestId_ = text(description, "requestId"); beginDescription(true);
        }
    }
    void recallLatest() { recallMessage(); }
    QJsonObject setSearchIntent(const QJsonObject& cue)
    {
        const QString intent = text(cue, "intent");
        const auto revision = cue.value(QStringLiteral("draftRevision"));
        const bool same = cue.size() == 6 && text(cue, "guestId") == guestId_ && text(cue, "composerId") == composerId_
            && revision.isDouble() && revision.toDouble() == double(draftRevision_)
            && cue.value(QStringLiteral("text")).isString() && text(cue, "text") == message_->toPlainText()
            && text(cue, "editorMode") == (selectedTrack_.isEmpty() ? QStringLiteral("message") : QStringLiteral("dedication"));
        if (!same || (intent != QStringLiteral("chat") && intent != QStringLiteral("music") && intent != QStringLiteral("auto")))
            return {{QStringLiteral("applied"), false}, {QStringLiteral("error"), QStringLiteral("Composer identity, revision, text or mode changed.")}};
        searchOverride_ = intent == QStringLiteral("auto") ? QString() : intent;
        cancelSearch(); ++searchVersion_; searchDelay_.stop(); cancelDescription(); cancelResultArtwork(); scheduleSearch();
        if (intent == QStringLiteral("music")) { searchDelay_.stop(); search(); }
        return {{QStringLiteral("applied"), true}, {QStringLiteral("effectiveIntent"), searchIntent()}};
    }
    void focusSearch()
    {
        if (selectedTrack_.isEmpty() && message_->toPlainText().trimmed().size() >= 2) {
            searchOverride_ = QStringLiteral("music"); cancelSearch(); ++searchVersion_; searchDelay_.stop(); cancelDescription(); scheduleSearch();
            searchDelay_.stop(); search();
        }
        message_->setFocus();
    }
protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == chat_->viewport() && event->type() == QEvent::ToolTip) {
            auto* help = static_cast<QHelpEvent*>(event);
            const QString anchor = chat_->anchorAt(help->pos());
            if (anchor.startsWith(QStringLiteral("work:"))) {
                const QString id = anchor.mid(5);
                QToolTip::showText(help->globalPos(), workTooltip(messageWork_.value(id).toObject()), chat_->viewport());
                return true;
            }
        }
        if (event->type() == QEvent::CursorChange) {
            if (auto* widget = qobject_cast<QWidget*>(watched); widget && widget->cursor().shape() != Qt::BitmapCursor)
                widget->setCursor(djCursor(widget->cursor().shape()));
        }
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::Wheel || event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease
            || (event->type() == QEvent::MouseMove && static_cast<QMouseEvent*>(event)->buttons() != Qt::NoButton)) {
            idleClock_.restart(); chatScroll_->stop(); followingBottom_ = false;
        }
        if ((watched == message_ || watched == chat_) && (event->type() == QEvent::Move || event->type() == QEvent::Resize))
            updateSuggestionOverlay();
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            auto* toggle = window()->findChild<QAction*>(QStringLiteral("partySpotifyAction"));
            if (toggle && toggle->isEnabled() && toggle->shortcut().matches(QKeySequence(key->keyCombination())) == QKeySequence::ExactMatch) {
                if (!key->isAutoRepeat()) toggle->trigger();
                return true;
            }
        }
        if (watched == message_ && event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            const auto modifiers = key->modifiers() & ~Qt::KeypadModifier;
            if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && modifiers == Qt::NoModifier) {
                sendMagic(); return true;
            }
        }
        return QDockWidget::eventFilter(watched, event);
    }
private:
    QJsonObject djPresence() const
    {
        const qint64 remaining = std::max<qint64>(0, presenceDeadline_ - presenceClock_.elapsed());
        return {{QStringLiteral("status"), !presenceKnown_ ? QStringLiteral("unavailable") : remaining > 0 ? QStringLiteral("online") : QStringLiteral("offline")},
                {QStringLiteral("remainingMs"), remaining}, {QStringLiteral("activeCount"), remaining > 0 ? presenceActiveCount_ : 0}};
    }
    void observeDjPresence(const QJsonObject& dj)
    {
        const auto llm = dj.value(QStringLiteral("llm")).toObject();
        if (llm == observedPresence_ && presenceSeen_) { updateDjPresence(); return; }
        observedPresence_ = llm; presenceSeen_ = true; presenceKnown_ = false; presenceDeadline_ = 0; presenceActiveCount_ = 0;
        const QString status = text(llm, "status");
        if (status == QStringLiteral("offline")) presenceKnown_ = true;
        else if (status == QStringLiteral("online")) {
            const auto last = llm.value(QStringLiteral("lastSeen")), expiry = llm.value(QStringLiteral("expiresAt"));
            const auto remaining = llm.value(QStringLiteral("remainingMs")), count = llm.value(QStringLiteral("activeCount"));
            const double now = QDateTime::currentMSecsSinceEpoch();
            const bool valid = last.isDouble() && expiry.isDouble() && remaining.isDouble() && count.isDouble()
                && std::isfinite(last.toDouble()) && last.toDouble() > 0 && last.toDouble() <= now
                && std::isfinite(expiry.toDouble()) && expiry.toDouble() >= last.toDouble() + 30000 && expiry.toDouble() <= last.toDouble() + 120000
                && std::isfinite(remaining.toDouble()) && remaining.toDouble() >= 0 && remaining.toDouble() <= expiry.toDouble() - last.toDouble()
                && count.toDouble() >= 1 && count.toDouble() <= 256 && count.toDouble() == std::floor(count.toDouble());
            if (valid) {
                presenceKnown_ = true; presenceActiveCount_ = count.toInt();
                const double lease = std::clamp(std::min(remaining.toDouble(), expiry.toDouble() - now), 0.0, 120000.0);
                presenceDeadline_ = presenceClock_.elapsed() + static_cast<qint64>(lease);
                // Metadata or wall-clock changes cannot renew an unchanged lease.
                // Keep this watermark through an intervening unavailable/offline
                // payload; a new service gets a separate watermark in setBase.
                if (expiry.toDouble() == presenceExpiryWatermark_)
                    presenceDeadline_ = std::min(presenceDeadline_, presenceWatermarkDeadline_);
                presenceExpiryWatermark_ = expiry.toDouble(); presenceWatermarkDeadline_ = presenceDeadline_;
            }
        }
        updateDjPresence();
    }
    void updateDjPresence()
    {
        if (!onlineBadge_) return;
        const auto state = djPresence(); const QString status = text(state, "status");
        const bool online = status == QStringLiteral("online");
        onlineBadge_->setText(online ? QStringLiteral("● DJ online") : status == QStringLiteral("offline") ? QStringLiteral("○ DJ offline") : QStringLiteral("○ DJ nedostupan"));
        onlineBadge_->setStyleSheet(QStringLiteral("font:%1px 'DejaVu Sans'; color:%2;").arg(UiMetrics::secondary).arg(online ? QStringLiteral("#98e7c8") : QStringLiteral("#b1bdcf")));
        onlineBadge_->setToolTip(online ? QStringLiteral("LLM DJ je potvrdio dostupnost. Potvrda vrijedi još %1 s.").arg(static_cast<int>(std::ceil(number(state, "remainingMs", 0) / 1000)))
            : QStringLiteral("Nema važeće potvrde dostupnosti LLM DJ-a."));
    }
    void invalidateSearchIntent() { cancelSearch(); ++draftRevision_; searchOverride_.clear(); }
    QString automaticSearchIntent() const
    {
        const QString words = message_->toPlainText().simplified().toCaseFolded();
        if (!selectedTrack_.isEmpty() || words.size() < 2) return QStringLiteral("chat");
        if (words.startsWith(QStringLiteral("spotify:track:"))) return QStringLiteral("music");
        // Fast local intent gating is deliberately cheap. Unknown concise titles
        // still go straight to the catalogue; Ctrl+K can explicitly search any text.
        static const QRegularExpression controls(QStringLiteral("\\b(?:gumb\\p{L}*|font\\p{L}*|dpi|scroll\\p{L}*|cursor\\p{L}*|kursor\\p{L}*|pokaziva\\p{L}*|popup\\p{L}*|pop.?up|sučelj\\p{L}*|sucelj\\p{L}*|layout|margin\\p{L}*|tipk\\p{L}*|shortcut\\p{L}*|ctrl|panel\\p{L}*|qtra|interface|button\\p{L}*|slova|poruk\\p{L}*|llm)\\b"), QRegularExpression::UseUnicodePropertiesOption);
        if (controls.match(words).hasMatch()) return QStringLiteral("chat");
        static const QRegularExpression music(QStringLiteral("\\b(?:pjesm\\p{L}*|song\\p{L}*|izvođa\\p{L}*|izvodja\\p{L}*|artist\\p{L}*|album\\p{L}*|refren\\p{L}*|stih\\p{L}*|lyrics|pusti|sviraj|play|karaoke)\\b"), QRegularExpression::UseUnicodePropertiesOption);
        if (music.match(words).hasMatch()) return QStringLiteral("music");
        static const QRegularExpression conversation(QStringLiteral("^(?:hvala|super|dobro|ok|zašto|zasto|kako|možeš|mozes|molim te|ovo je|ovo mi|nije mi|čini se|cini se|can you|please|why|thank)\\b"), QRegularExpression::UseUnicodePropertiesOption);
        return conversation.match(words).hasMatch() || words.count(QChar(' ')) >= 8 || message_->toPlainText().contains(QChar('\n'))
            ? QStringLiteral("chat") : QStringLiteral("music");
    }
    QString searchIntent() const
    {
        if (!selectedTrack_.isEmpty()) return QStringLiteral("chat");
        return searchOverride_.isEmpty() ? automaticSearchIntent() : searchOverride_;
    }
    void applyEditorDraft(const QJsonObject& draft)
    {
        selectedTrack_ = draft.value(QStringLiteral("selectedTrack")).toObject();
        if (text(selectedTrack_, "uri").isEmpty()) selectedTrack_ = {};
        savedQuery_ = text(draft, "query");
        savedQueryCursor_ = static_cast<int>(number(draft, "queryCursor", savedQuery_.size()));
        savedQueryAnchor_ = static_cast<int>(number(draft, "queryAnchor", savedQueryCursor_));
        setComposerText(text(draft, "text"), static_cast<int>(number(draft, "cursor", text(draft, "text").size())),
                        static_cast<int>(number(draft, "anchor", number(draft, "cursor", text(draft, "text").size()))));
        updateComposerMode(); composerChanged();
    }
    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
    QString name() const { return name_->text().trimmed().isEmpty() ? QStringLiteral("Gost") : name_->text().trimmed(); }
    void animateChatToBottom(int duration = 450)
    {
        auto* scroll = chat_->verticalScrollBar();
        chatScroll_->stop(); followingBottom_ = true;
        if (scroll->value() >= scroll->maximum()) return;
        chatScroll_->setDuration(duration); chatScroll_->setStartValue(scroll->value()); chatScroll_->setEndValue(scroll->maximum());
        chatScroll_->start();
    }
    void updateSuggestionOverlay()
    {
        // This child floats over the transcript and never participates in its
        // layout. Showing results or a selected song cannot move the composer.
        const QRect composer = message_->geometry();
        const QRect chat = chat_->geometry();
        const int height = std::max(1, std::min(340, chat.height()));
        suggestionArea_->setFixedHeight(height);
        suggestionArea_->setGeometry(composer.x(), std::max(chat.y(), composer.y() - 11 - height), composer.width(), height);
        const bool compact = height < 230;
        selectedTitle_->setFixedHeight(compact ? 36 : 73);
        auto* selectedLayout = selectedPage_->layout();
        selectedLayout->setContentsMargins(15, compact ? 6 : 13, 15, compact ? 6 : 13);
        selectedLayout->setSpacing(compact ? 4 : 6);
        const bool selected = !selectedTrack_.isEmpty();
        const bool results = results_->count() > 0 &&
            (suggestionState_ == QStringLiteral("results") || suggestionState_ == QStringLiteral("loading"));
        suggestionArea_->setVisible(selected || results || descriptionActive_);
        if (selected || results || descriptionActive_) suggestionArea_->raise();
        notice_->setGeometry(chat.x() + 8, chat.y() + 8,
                             std::max(1, chat.width() - 16), std::min(88, chat.height() - 16));
        if (notice_->isVisible()) notice_->raise();
    }
    void setNotice(const QString& words)
    {
        notice_->setToolTip(words);
        const int lineWidth = std::max(120, chat_->width() - 54);
        notice_->setText(QFontMetrics(stageFont(26)).elidedText(words, Qt::ElideRight, lineWidth * 2));
        notice_->setVisible(!words.isEmpty());
        if (words.isEmpty()) noticeDismiss_.stop();
        else { updateSuggestionOverlay(); notice_->raise(); noticeDismiss_.start(); }
    }
    void suggestionHint(const QString& words, const QString& state)
    {
        suggestionHint_->setText(words); suggestionHint_->setToolTip(words);
        suggestions_->setCurrentWidget(suggestionHint_); suggestionState_ = state;
        updateSuggestionOverlay();
    }
    void setComposerText(const QString& words, int position, int anchor)
    {
        ++composerGeneration_; invalidateSearchIntent();
        const QSignalBlocker block(message_);
        message_->setPlainText(words);
        auto cursor = message_->textCursor();
        cursor.setPosition(std::clamp(anchor, 0, static_cast<int>(words.size())));
        cursor.setPosition(std::clamp(position, 0, static_cast<int>(words.size())), QTextCursor::KeepAnchor);
        message_->setTextCursor(cursor);
    }
    void updateComposerMode()
    {
        const bool song = !selectedTrack_.isEmpty();
        message_->setPlaceholderText(song ? QStringLiteral("Posveta uz pjesmu (po želji)…")
                                         : QStringLiteral("Magic box: pjesma, izvođač ili poruka DJ-u…"));
        magicHelp_->setText(QStringLiteral("<b style=\"color:#7de4ed\">%1: qtra ↔ Spotify</b><br>Shift+Enter: novi red · Esc: vrati")
                           .arg(shortcutKey(GuestShortcut::Spotify).toHtmlEscaped()));
        send_->setText(song ? QStringLiteral("Pošalji pjesmu (Enter)") : QStringLiteral("Pošalji poruku (Enter)"));
        const bool busy = requestBusy_ || messageBusy_ || recallBusy_;
        send_->setEnabled(!busy && (song || !message_->toPlainText().trimmed().isEmpty()));
        cancelSong_->setEnabled(!busy);
        describe_->setEnabled(!busy && !song && !descriptionBusy_
            && !(descriptionActive_ && descriptionStatus_ == QStringLiteral("pending"))
            && message_->toPlainText().trimmed().size() >= 3 && message_->toPlainText().trimmed().size() <= 1500);
        alternate_->setEnabled(!busy && !alternateDraft_.isEmpty());
        alternate_->setText(alternateLabel_);
        if (song) {
            searchActivity_->setRange(0, 1); searchActivity_->setValue(0);
            const QFontMetrics fm(stageFont(UiMetrics::body));
            selectedTitle_->setText(fm.elidedText(text(selectedTrack_, "title"), Qt::ElideRight, 500));
            selectedTitle_->setToolTip(trackTooltip(selectedTrack_));
            selectedArtist_->setText(fm.elidedText(text(selectedTrack_, "artist"), Qt::ElideRight, 500));
            selectedArtist_->setToolTip(text(selectedTrack_, "artist"));
            suggestions_->setCurrentWidget(selectedPage_); suggestionState_ = QStringLiteral("selected");
        }
        updateSuggestionOverlay();
    }
    void composerChanged()
    {
        ++composerGeneration_; invalidateSearchIntent(); ++searchVersion_; cancelDescription(); cancelResultArtwork(); updateComposerMode(); scheduleSearch();
    }
    void scheduleSearch()
    {
        if (!selectedTrack_.isEmpty()) { searchDelay_.stop(); return; }
        if (message_->toPlainText().trimmed().size() < 2 || searchIntent() == QStringLiteral("chat")) {
            { const QSignalBlocker block(results_); results_->clear(); }
            results_->setEnabled(true); searchActivity_->setRange(0, 1); searchActivity_->setValue(0);
            searchDelay_.stop();
            suggestionHint(QStringLiteral("Poruku možeš odmah poslati DJ-u. %1 traži pjesmu.").arg(shortcutKey(GuestShortcut::Search)), QStringLiteral("empty"));
        } else {
            // Keep the current page and rows still while the query changes.
            // Stale choices stay visible but cannot be selected until replaced.
            results_->setEnabled(false); searchActivity_->setRange(0, 0);
            if (suggestions_->currentWidget() == selectedPage_) suggestions_->setCurrentWidget(results_);
            suggestionState_ = QStringLiteral("loading");
            updateSuggestionOverlay();
            searchDelay_.start();
        }
    }
    void clearSearchResults()
    {
        searchDelay_.stop(); cancelResultArtwork();
        { const QSignalBlocker block(results_); results_->clear(); }
        results_->setEnabled(true); searchActivity_->setRange(0, 1); searchActivity_->setValue(0);
        if (selectedTrack_.isEmpty()) suggestionHint(QString(), QStringLiteral("empty"));
        else updateSuggestionOverlay();
    }
    void chooseSong()
    {
        if (requestBusy_ || messageBusy_ || recallBusy_ || !selectedTrack_.isEmpty() || !results_->currentItem() || results_->selectedItems().isEmpty()) return;
        const auto track = QJsonObject::fromVariantMap(results_->currentItem()->data(Qt::UserRole).toMap());
        if (text(track, "uri").isEmpty()) return;
        savedQuery_ = message_->toPlainText();
        savedQueryCursor_ = message_->textCursor().position(); savedQueryAnchor_ = message_->textCursor().anchor();
        selectedTrack_ = track; ++searchVersion_; searchDelay_.stop(); cancelDescription(); setComposerText(QString(), 0, 0);
        cancelResultArtwork();
        updateComposerMode(); setNotice(QStringLiteral("Pjesma je odabrana. Možeš dodati posvetu.")); message_->setFocus();
    }
    void cancelSong()
    {
        if (requestBusy_ || messageBusy_ || recallBusy_) return;
        selectedTrack_ = {};
        setComposerText(savedQuery_, savedQueryCursor_, savedQueryAnchor_); savedQuery_.clear();
        updateComposerMode(); composerChanged(); setNotice(QStringLiteral("Pjesma nije poslana. Upit je vraćen.")); message_->setFocus();
    }
    void sendMagic()
    {
        if (requestBusy_ || messageBusy_ || recallBusy_) return;
        invalidateSearchIntent(); ++searchVersion_; cancelDescription(); clearSearchResults();
        selectedTrack_.isEmpty() ? sendMessage() : requestTrack();
    }
    using Done = std::function<void(QJsonObject, QString)>;
    QNetworkReply* call(const QString& path, const QJsonObject* data, Done done)
    {
        const QUrl endpoint = base_.resolved(QUrl(path));
        QNetworkRequest request(endpoint); request.setTransferTimeout(12000);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        auto* reply = data ? network_.post(request, QJsonDocument(*data).toJson(QJsonDocument::Compact)) : network_.get(request);
        connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
            const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
            const QJsonObject object = document.object();
            QString error = text(object, "error");
            if (error.isEmpty() && reply->error() != QNetworkReply::NoError) error = reply->errorString();
            if (error.isEmpty() && !document.isObject()) error = QStringLiteral("DJ service returned an unreadable response");
            done(object, error); reply->deleteLater();
        });
        return reply;
    }
    void cancelSearch()
    {
        if (activeSearchId_.isEmpty()) return;
        const QString id = activeSearchId_; const QUrl base = activeSearchBase_;
        activeSearchId_.clear();
        const auto reply = activeSearchReply_; activeSearchReply_.clear();
        if (reply && reply->isRunning()) reply->abort();
        // The cancellation outlives this desk during a stage-only hot reload.
        // It carries only the already-dispatched opaque request ID, never a draft.
        auto* cancellation = new QNetworkAccessManager(QCoreApplication::instance());
        QNetworkRequest request(base.resolved(QUrl(QStringLiteral("/api/search/cancel"))));
        request.setTransferTimeout(3000); request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        const QJsonObject data{{QStringLiteral("searchId"), id}};
        auto* cancelled = cancellation->post(request, QJsonDocument(data).toJson(QJsonDocument::Compact));
        connect(cancelled, &QNetworkReply::finished, cancellation, &QObject::deleteLater);
    }
    void search()
    {
        if (!selectedTrack_.isEmpty() || searchIntent() == QStringLiteral("chat")) return;
        const QString query = message_->toPlainText().simplified().left(160);
        if (query.size() < 2) return;
        cancelSearch();
        const quint64 version = searchVersion_; const QString id = uuid();
        activeSearchId_ = id; activeSearchBase_ = base_;
        QUrlQuery args; args.addQueryItem(QStringLiteral("q"), query); args.addQueryItem(QStringLiteral("searchId"), id);
        activeSearchReply_ = call(QStringLiteral("/api/search?") + args.toString(QUrl::FullyEncoded), nullptr, [this, version, id](QJsonObject data, QString error) {
            if (id != activeSearchId_ || version != searchVersion_ || !selectedTrack_.isEmpty()) return;
            if (error.isEmpty() && text(data, "searchId") != id) error = QStringLiteral("Odgovor pretrage ne pripada ovom upitu.");
            if (!error.isEmpty()) cancelSearch();
            else { activeSearchId_.clear(); activeSearchReply_.clear(); }
            results_->setEnabled(true); searchActivity_->setRange(0, 1); searchActivity_->setValue(0);
            if (error.isEmpty() && data.value(QStringLiteral("cancelled")).toBool()) { clearSearchResults(); return; }
            if (!error.isEmpty()) {
                { const QSignalBlocker block(results_); results_->clear(); }
                suggestionHint(QStringLiteral("Pretraga trenutačno ne radi.\nPoruku i dalje možeš poslati DJ-u."), QStringLiteral("error"));
                setNotice(error); return;
            }
            showTracks(data.value(QStringLiteral("tracks")).toArray());
        });
    }
    static QJsonObject matchDetails(const QJsonObject& track)
    {
        if (track.contains(QStringLiteral("matchBasis")))
            return {{QStringLiteral("basis"), track.value(QStringLiteral("matchBasis"))},
                    {QStringLiteral("rationale"), track.value(QStringLiteral("rationale"))},
                    {QStringLiteral("confidence"), track.value(QStringLiteral("confidence"))},
                    {QStringLiteral("sources"), track.value(QStringLiteral("sources"))}};
        return track.value(QStringLiteral("match")).toObject();
    }
    static QString matchBasis(const QJsonObject& track)
    {
        const QString basis = text(matchDetails(track), "basis");
        if (basis == QStringLiteral("title")) return QStringLiteral("Po naslovu");
        if (basis == QStringLiteral("literal_lyrics")) return QStringLiteral("Doslovni stih");
        if (basis == QStringLiteral("semantic")) return QStringLiteral("Po značenju");
        return {};
    }
    static QString trackTooltip(const QJsonObject& track)
    {
        QString html = QStringLiteral("<qt><b>%1</b><br>%2")
            .arg(text(track, "title").toHtmlEscaped(), text(track, "artist").toHtmlEscaped());
        const QString basis = matchBasis(track);
        if (!basis.isEmpty()) {
            const auto match = matchDetails(track);
            html += QStringLiteral("<br><br><b>%1</b><br>%2").arg(basis,
                text(match, "rationale").left(1200).toHtmlEscaped().replace(QChar('\n'), QStringLiteral("<br>")));
            const auto confidence = match.value(QStringLiteral("confidence"));
            if (confidence.isDouble() && std::isfinite(confidence.toDouble()) && confidence.toDouble() >= 0 && confidence.toDouble() <= 1)
                html += QStringLiteral("<br>Procjena podudaranja: %1%").arg(qRound(confidence.toDouble() * 100));
            int shown = 0;
            for (const auto& value : match.value(QStringLiteral("sources")).toArray()) {
                const auto source = value.toObject(); const QUrl url(text(source, "url"), QUrl::StrictMode);
                if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()
                    || (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http"))) continue;
                html += QStringLiteral("<br><br>%1<br>%2").arg(text(source, "label", url.host()).left(160).toHtmlEscaped(),
                    url.toDisplayString(QUrl::FullyEncoded).toHtmlEscaped());
                if (++shown == 4) break;
            }
        }
        return html + QStringLiteral("</qt>");
    }
    void showTracks(const QJsonArray& tracks, const QString& note = {})
    {
        cancelResultArtwork();
        const QSignalBlocker block(results_); results_->clear(); results_->setEnabled(true);
        const QFontMetrics fm(stageFont(UiMetrics::body)); const int width = std::max(100, suggestions_->width() - 140);
        QPixmap placeholder(72, 72); placeholder.fill(QColor("#2b566d"));
        { QPainter p(&placeholder); p.setPen(QColor("#b7e7ef")); p.setFont(stageFont(UiMetrics::icon)); p.drawText(placeholder.rect(), Qt::AlignCenter, QStringLiteral("♪")); }
        for (const auto& value : tracks) {
            if (results_->count() >= 20) break;
            const QJsonObject track = value.toObject();
            if (text(track, "uri").isEmpty()) continue;
            const QString basis = matchBasis(track);
            QString label = fm.elidedText(text(track, "title"), Qt::ElideRight, width)
                + QStringLiteral("\n") + fm.elidedText(text(track, "artist"), Qt::ElideRight, width);
            if (!basis.isEmpty()) label += QStringLiteral("\n") + basis;
            auto* item = new QListWidgetItem(label, results_);
            const QString artwork = coverUrl(track);
            const QImage* cached = resultArtCache_.object(artwork);
            item->setIcon(QIcon(cached ? QPixmap::fromImage(*cached) : placeholder));
            item->setData(Qt::UserRole + 1, cached != nullptr);
            item->setData(Qt::UserRole, track.toVariantMap());
            item->setToolTip(trackTooltip(track));
            item->setSizeHint(QSize(0, basis.isEmpty() ? 95 : 128));
            if (!artwork.isEmpty() && !cached && !resultArtQueue_.contains(artwork)) resultArtQueue_.enqueue(artwork);
        }
        results_->setCurrentRow(-1);
        if (results_->count()) {
            suggestions_->setCurrentWidget(results_); suggestionState_ = QStringLiteral("results"); updateSuggestionOverlay();
        } else suggestionHint(note.isEmpty() ? QStringLiteral("Nema pjesama za taj upit.\nPoruku možeš poslati DJ-u.") : note,
                              QStringLiteral("empty"));
        if (!note.isEmpty()) setNotice(note);
        fetchResultArtwork();
    }
    void cancelResultArtwork()
    {
        ++resultArtVersion_; resultArtQueue_.clear();
        const auto pending = resultArtReplies_; resultArtReplies_.clear();
        for (const auto& reply : pending) if (reply) reply->abort();
    }
    void fetchResultArtwork()
    {
        while (resultArtReplies_.size() < 2 && !resultArtQueue_.isEmpty()) {
            const QString url = resultArtQueue_.dequeue(); const quint64 version = resultArtVersion_;
            QNetworkRequest request{QUrl(url)}; request.setTransferTimeout(8000);
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            auto* reply = resultArtNetwork_.get(request); resultArtReplies_.insert(url, reply);
            auto bytes = std::make_shared<QByteArray>(); auto oversized = std::make_shared<bool>(false);
            const auto consume = [this, reply, version, bytes, oversized] {
                if (version != resultArtVersion_ || *oversized) return;
                constexpr qsizetype limit = 2 * 1024 * 1024;
                *bytes += reply->read(std::max<qsizetype>(0, limit + 1 - bytes->size()));
                if (bytes->size() > limit) { *oversized = true; reply->abort(); }
            };
            connect(reply, &QNetworkReply::readyRead, this, consume);
            connect(reply, &QNetworkReply::finished, this, [this, reply, url, version, bytes, oversized, consume] {
                if (version != resultArtVersion_) { reply->deleteLater(); return; }
                consume(); resultArtReplies_.remove(url);
                if (!*oversized && reply->error() == QNetworkReply::NoError) {
                    QString error; const QImage image = decodeCover(*bytes, 144, error);
                    if (!image.isNull()) {
                        resultArtCache_.insert(url, new QImage(image));
                        for (int i = 0; i < results_->count(); ++i) {
                            auto* item = results_->item(i);
                            if (coverUrl(QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap())) == url) {
                                item->setIcon(QIcon(QPixmap::fromImage(image))); item->setData(Qt::UserRole + 1, true);
                            }
                        }
                    }
                }
                reply->deleteLater(); fetchResultArtwork();
            });
        }
    }
    void cancelDescription()
    {
        descriptionPoll_.stop(); descriptionActive_ = false; descriptionBusy_ = false;
        descriptionId_.clear(); descriptionStatus_.clear();
    }
    void applyDescriptionReply(const QJsonObject& data, const QString& error)
    {
        descriptionBusy_ = false;
        const auto search = data.value(QStringLiteral("search")).toObject();
        const QString id = search.value(QStringLiteral("id")).toVariant().toString(), status = text(search, "status");
        if (!error.isEmpty() || id.isEmpty() || (status != QStringLiteral("pending") && status != QStringLiteral("resolved"))
            || (!descriptionId_.isEmpty() && descriptionId_ != id)) {
            descriptionPoll_.stop(); descriptionStatus_ = QStringLiteral("error");
            searchActivity_->setRange(0, 1); searchActivity_->setValue(0);
            suggestionHint(QStringLiteral("Pretraga po opisu nije dovršena.\nMožeš pokušati ponovno ili urediti upit."), QStringLiteral("description-error"));
            setNotice(error.isEmpty() ? QStringLiteral("DJ je vratio nečitljiv odgovor pretrage.") : error);
            updateComposerMode(); return;
        }
        descriptionId_ = id; descriptionStatus_ = status;
        if (status == QStringLiteral("pending")) {
            const QString note = text(search, "note");
            suggestionHint(note.isEmpty() ? QStringLiteral("DJ traži pjesmu po opisu…\nRezultati će se pojaviti ovdje.") : note,
                           QStringLiteral("description-pending"));
            searchActivity_->setRange(0, 0); descriptionPoll_.start();
        } else {
            descriptionPoll_.stop(); searchActivity_->setRange(0, 1); searchActivity_->setValue(0);
            showTracks(search.value(QStringLiteral("tracks")).toArray(), text(search, "note"));
        }
        updateComposerMode();
    }
    void beginDescription(bool restoring = false)
    {
        const QString words = message_->toPlainText().trimmed();
        if (words.size() < 3 || words.size() > 1500 || !selectedTrack_.isEmpty() || requestBusy_ || messageBusy_ || recallBusy_ || descriptionBusy_) return;
        searchOverride_ = QStringLiteral("music");
        if (!restoring && !(descriptionStatus_ == QStringLiteral("error") && descriptionText_ == words)) descriptionRequestId_ = uuid();
        if (descriptionRequestId_.isEmpty()) descriptionRequestId_ = uuid();
        cancelSearch(); cancelResultArtwork(); ++searchVersion_; searchDelay_.stop(); descriptionPoll_.stop();
        descriptionText_ = words; descriptionId_.clear(); descriptionStatus_ = QStringLiteral("pending");
        descriptionActive_ = true; descriptionBusy_ = true; results_->setEnabled(false);
        suggestionHint(QStringLiteral("DJ traži pjesmu po opisu…\nRezultati će se pojaviti ovdje."), QStringLiteral("description-pending"));
        searchActivity_->setRange(0, 0); updateComposerMode();
        const quint64 version = searchVersion_;
        const QJsonObject data{{QStringLiteral("guestId"), guestId_}, {QStringLiteral("requestId"), descriptionRequestId_},
                              {QStringLiteral("text"), words}};
        call(QStringLiteral("/api/search/describe"), &data, [this, version](QJsonObject data, QString error) {
            if (version != searchVersion_ || !descriptionActive_ || !selectedTrack_.isEmpty()) return;
            applyDescriptionReply(data, error);
        });
    }
    void pollDescription()
    {
        if (!descriptionActive_ || descriptionBusy_ || descriptionId_.isEmpty() || !selectedTrack_.isEmpty()) return;
        descriptionBusy_ = true; const quint64 version = searchVersion_; const QString id = descriptionId_;
        QUrlQuery query; query.addQueryItem(QStringLiteral("id"), id);
        call(QStringLiteral("/api/search/describe?") + query.toString(QUrl::FullyEncoded), nullptr,
             [this, version, id](QJsonObject data, QString error) {
            if (version != searchVersion_ || !descriptionActive_ || id != descriptionId_ || !selectedTrack_.isEmpty()) return;
            applyDescriptionReply(data, error);
        });
    }
    void requestTrack()
    {
        if (requestBusy_ || selectedTrack_.isEmpty()) return;
        const QString uri = text(selectedTrack_, "uri"); if (uri.isEmpty()) return;
        const QString words = message_->toPlainText().trimmed();
        if (words.size() > 240) { setNotice(QStringLiteral("Posveta može imati do 240 znakova.")); return; }
        const QString signature = uri + QChar('\n') + name() + QChar('\n') + words;
        if (signature != lastRequestSignature_) { requestId_ = uuid(); lastRequestSignature_ = signature; }
        QJsonObject data{{QStringLiteral("requestId"), requestId_}, {QStringLiteral("uri"), uri},
                         {QStringLiteral("requestedBy"), name()}, {QStringLiteral("dedication"), words}};
        requestBusy_ = true; updateComposerMode(); setNotice(QStringLiteral("Šaljem pjesmu i posvetu…"));
        call(QStringLiteral("/api/request"), &data, [this, title = text(selectedTrack_, "title"), words](QJsonObject data, QString error) {
            requestBusy_ = false;
            if (!error.isEmpty()) { updateComposerMode(); setNotice(error); return; }
            const QString status = text(data, "status", text(data.value(QStringLiteral("request")).toObject(), "status"));
            setNotice(title + (status == QStringLiteral("queued") ? QStringLiteral(" — dodano u red.") : QStringLiteral(" — zahtjev je stigao DJ-u.")));
            selectedTrack_ = {}; savedQuery_.clear(); lastRequestSignature_.clear();
            if (message_->toPlainText().trimmed() == words) setComposerText(QString(), 0, 0);
            updateComposerMode(); composerChanged();
        });
    }
    void appendMessages(const QJsonArray& incoming, bool changed = false, bool followBottom = false)
    {
        bool conversationActivity = false;
        for (const auto& value : incoming) {
            const auto message = value.toObject(); const QString id = message.value(QStringLiteral("id")).toVariant().toString();
            if (id.isEmpty() || withdrawnIds_.contains(id)) continue;
            if (messageIds_.contains(id)) {
                for (qsizetype i = 0; i < messages_.size(); ++i) {
                    const auto previous = messages_[i].toObject();
                    if (previous.value(QStringLiteral("id")).toVariant().toString() == id) {
                        if (previous != message) {
                            conversationActivity |= text(previous, "text") != text(message, "text");
                            messages_[i] = message; changed = true;
                        }
                        break;
                    }
                }
                continue;
            }
            messageIds_.insert(id); messages_.append(message); changed = true; conversationActivity = true;
        }
        if (conversationActivity) idleClock_.restart();
        if (!changed) {
            if (followBottom) animateChatToBottom();
            return;
        }
        auto* scroll = chat_->verticalScrollBar();
        const bool atBottom = followBottom || scroll->value() >= scroll->maximum() - 8 || followingBottom_;
        chatScroll_->stop(); followingBottom_ = false;
        const int oldScroll = scroll->value(); QString html;
        QList<QJsonObject> ordered;
        for (const auto& value : messages_) ordered.append(value.toObject());
        std::stable_sort(ordered.begin(), ordered.end(), [](const QJsonObject& a, const QJsonObject& b) {
            const double at = number(a, "createdAt", 0), bt = number(b, "createdAt", 0);
            if (at != bt) return at < bt;
            return a.value(QStringLiteral("id")).toVariant().toLongLong() < b.value(QStringLiteral("id")).toVariant().toLongLong();
        });
        for (const auto& message : ordered) {
            const bool dj = text(message, "role") == QStringLiteral("assistant");
            const QString author = dj ? QStringLiteral("DJ") : text(message, "name", QStringLiteral("Gost"));
            const QString id = message.value(QStringLiteral("id")).toVariant().toString();
            const QString receipt = dj ? QString() : QStringLiteral(" <span style='font-family:monospace;color:%1'>%2</span>")
                .arg(text(message, "status") == QStringLiteral("read") ? QStringLiteral("#8994a6") : QStringLiteral("#080f1d"),
                     text(message, "status") == QStringLiteral("read") ? QStringLiteral("✓") : QStringLiteral("&nbsp;"));
            QString work;
            if (!dj) {
                const bool recorded = messageWork_.contains(id);
                const QString state = recorded ? effectiveWork(messageWork_.value(id).toObject())
                    : processingIds_.contains(id) ? QStringLiteral("active") : QString();
                QString mark = QStringLiteral("&nbsp;"), colour = QStringLiteral("#080f1d");
                if (state == QStringLiteral("active")) { mark = QStringLiteral("●"); colour = QStringLiteral("#ffb16c"); }
                else if (state == QStringLiteral("waiting")) { mark = QStringLiteral("◷"); colour = QStringLiteral("#a8b9cb"); }
                else if (state == QStringLiteral("blocked")) { mark = QStringLiteral("◇"); colour = QStringLiteral("#f2c476"); }
                else if (state == QStringLiteral("unconfirmed")) { mark = QStringLiteral("?"); colour = QStringLiteral("#f2c476"); }
                else if (state == QStringLiteral("completed")) { mark = QStringLiteral("◆"); colour = QStringLiteral("#8ed8bd"); }
                else if (state == QStringLiteral("cancelled")) { mark = QStringLiteral("–"); colour = QStringLiteral("#8593a6"); }
                work = recorded ? QStringLiteral(" <a href='work:%1' style='font-family:monospace;color:%2;text-decoration:none'>%3</a>").arg(id.toHtmlEscaped(), colour, mark)
                    : QStringLiteral(" <span style='font-family:monospace;color:%1'>%2</span>").arg(colour, mark);
            }
            const QString recall = dj ? QString() : canRecall(message)
                ? QStringLiteral(" <a href='recall:%1' style='color:#b3f4fa;font-family:monospace;font-size:%2px;font-weight:bold;text-decoration:none'>&nbsp;×&nbsp;</a>").arg(lastSentId_).arg(UiMetrics::icon)
                : QStringLiteral(" <span style='font-family:monospace;font-size:%1px'>&nbsp;&nbsp;&nbsp;</span>").arg(UiMetrics::icon);
            html += QStringLiteral("<p style='margin:8px 0 13px'><b style='color:%1'>%2</b>%3%4%5<br><span style='color:#f0f2f7'>%6</span></p>")
                .arg(dj ? QStringLiteral("#ff9a58") : QStringLiteral("#7de4ed"), author.toHtmlEscaped(), receipt,
                     work, recall,
                     text(message, "text").toHtmlEscaped().replace(QChar('\n'), QStringLiteral("<br>")));
        }
        chat_->setHtml(html); scroll->setValue(oldScroll); workPresentation_ = workStates();
        if (atBottom) animateChatToBottom();
    }
    void fetchChat()
    {
        if (chatBusy_) return;
        chatBusy_ = true;
        const quint64 service = serviceGeneration_;
        QString path = QStringLiteral("/api/chat");
        const bool receipts = !cursor_.isEmpty() && (++chatFetches_ % 2 == 0);
        if (!cursor_.isEmpty() && !receipts) { QUrlQuery query; query.addQueryItem(QStringLiteral("after"), cursor_); path += QChar('?') + query.toString(QUrl::FullyEncoded); }
        call(path, nullptr, [this, receipts, service](QJsonObject data, QString error) {
            if (service != serviceGeneration_) return;
            chatBusy_ = false;
            if (!error.isEmpty()) { presence_->setText(QStringLiteral("DJ veza: ") + error); observeDjPresence({}); return; }
            const auto incoming = data.value(QStringLiteral("messages")).toArray();
            const bool withdrawn = withdrawMessages(data.value(QStringLiteral("withdrawnMessageIds")).toArray());
            const auto dj = data.value(QStringLiteral("dj")).toObject();
            observeDjPresence(dj);
            const bool workChanged = mergeWork(incoming, dj.value(QStringLiteral("messageWork")).toObject());
            QSet<QString> processing;
            for (const auto& id : dj.value(QStringLiteral("processingMessageIds")).toArray()) processing.insert(id.toVariant().toString());
            const bool processingChanged = processing != processingIds_; processingIds_ = processing;
            appendMessages(incoming, processingChanged || withdrawn || workChanged);
            // Only a fetched history response advances the read cursor. A POST
            // can finish while older replies are still in flight.
            // Receipt refreshes revisit recent IDs without skipping any IDs that
            // still need to arrive through the incremental history cursor.
            if (!receipts && !incoming.isEmpty()) cursor_ = incoming.last().toObject().value(QStringLiteral("id")).toVariant().toString();
            presence_->setText(activeWorkCount() == 0
                ? QStringLiteral("Poruke i zahtjevi DJ-u")
                : QStringLiteral("DJ radi na poruci…"));
        });
    }
    void sendMessage()
    {
        const QString words = message_->toPlainText().trimmed(); if (messageBusy_ || words.isEmpty()) return;
        if (words.size() > 1500) { setNotice(QStringLiteral("Poruka može imati do 1500 znakova.")); return; }
        if (words != lastMessage_) { messageId_ = uuid(); lastMessage_ = words; }
        const QJsonObject data{{QStringLiteral("guestId"), guestId_}, {QStringLiteral("name"), name()},
                              {QStringLiteral("text"), words}, {QStringLiteral("requestId"), messageId_},
                              {QStringLiteral("source"), QStringLiteral("native")}};
        const quint64 generation = composerGeneration_;
        messageBusy_ = true; updateComposerMode();
        call(QStringLiteral("/api/message"), &data, [this, words, generation](QJsonObject data, QString error) {
            messageBusy_ = false; updateComposerMode();
            if (!error.isEmpty()) { setNotice(error); return; }
            const auto sent = data.value(QStringLiteral("message")).toObject();
            lastSentId_ = static_cast<qint64>(number(sent, "id", 0));
            appendMessages(QJsonArray{sent}, false, true);
            if (generation == composerGeneration_ && message_->toPlainText().trimmed() == words) message_->clear();
            lastMessage_.clear(); setNotice(QString()); fetchChat();
        });
    }
    bool withdrawMessages(const QJsonArray& ids)
    {
        bool changed = false;
        for (const auto& value : ids) {
            const QString id = value.toVariant().toString(); if (id.isEmpty()) continue;
            withdrawnIds_.insert(id);
            for (qsizetype i = messages_.size(); i-- > 0;) {
                if (messages_[i].toObject().value(QStringLiteral("id")).toVariant().toString() == id) {
                    messages_.removeAt(i); messageIds_.remove(id); changed = true;
                }
            }
        }
        if (changed) idleClock_.restart();
        return changed;
    }
    void restoreAlternateDraft()
    {
        if (alternateDraft_.isEmpty() || requestBusy_ || messageBusy_ || recallBusy_) return;
        const auto current = editorDraft(), alternate = alternateDraft_;
        alternateDraft_ = current; alternateLabel_ = QStringLiteral("Vrati skicu");
        applyEditorDraft(alternate); setNotice(QStringLiteral("Skica je vraćena. Druga je i dalje sačuvana.")); message_->setFocus();
    }
    bool canRecall(const QJsonObject& message) const
    {
        const QString id = message.value(QStringLiteral("id")).toVariant().toString();
        return lastSentId_ > 0 && id == QString::number(lastSentId_) && text(message, "role") == QStringLiteral("user")
            && (text(message, "status", QStringLiteral("delivered")) == QStringLiteral("delivered") || text(message, "status") == QStringLiteral("read"))
            && number(message, "acceptedAt", 0) <= 0 && !processingIds_.contains(id) && !messageWork_.contains(id)
            && !message.value(QStringLiteral("work")).isObject() && !withdrawnIds_.contains(id);
    }
    void recallMessage()
    {
        if (recallBusy_ || requestBusy_ || messageBusy_) return;
        if (lastSentId_ <= 0) { setNotice(QStringLiteral("Nema tvoje zadnje poslane poruke za povlačenje.")); return; }
        for (const auto& value : messages_) {
            const auto message = value.toObject();
            if (number(message, "id", 0) == lastSentId_ && !canRecall(message)) {
                setNotice(QStringLiteral("DJ je već preuzeo tu poruku. Skica je sačuvana.")); return;
            }
        }
        invalidateSearchIntent(); ++searchVersion_; cancelDescription(); clearSearchResults();
        const qint64 id = lastSentId_; const quint64 generation = composerGeneration_;
        const auto before = editorDraft(); const QUrl base = base_;
        const QPointer<QWidget> focus = QApplication::focusWidget();
        const QJsonObject data{{QStringLiteral("guestId"), guestId_}, {QStringLiteral("messageId"), id}};
        recallBusy_ = true; updateComposerMode();
        call(QStringLiteral("/api/message/recall"), &data, [this, id, generation, before, base, focus](QJsonObject data, QString error) {
            if (base != base_) return;
            recallBusy_ = false;
            const auto recalled = data.value(QStringLiteral("recalled")).toObject();
            if (!error.isEmpty() || number(recalled, "id", -1) != id || !recalled.value(QStringLiteral("text")).isString()) {
                setNotice(error.isEmpty() ? QStringLiteral("Poruka nije povučena. Skica je sačuvana.") : error); updateComposerMode(); return;
            }
            const QString words = text(recalled, "text");
            const QJsonObject recovered{{QStringLiteral("text"), words}, {QStringLiteral("cursor"), words.size()},
                                       {QStringLiteral("anchor"), words.size()}};
            const bool unchanged = generation == composerGeneration_ && before == editorDraft();
            const bool removed = withdrawMessages(QJsonArray{id}); appendMessages({}, removed);
            lastSentId_ = 0;
            if (unchanged) {
                alternateDraft_ = before; alternateLabel_ = QStringLiteral("Vrati skicu"); applyEditorDraft(recovered);
                setNotice(QStringLiteral("Poruka je povučena. Uredi je; prethodna skica je sačuvana."));
                if (QApplication::focusWidget() == focus.data()) message_->setFocus();
            } else {
                alternateDraft_ = recovered; alternateLabel_ = QStringLiteral("Uredi povučenu");
                setNotice(QStringLiteral("Poruka je povučena. Nova skica je ostala; „Uredi povučenu” otvara povučeni tekst."));
            }
            updateComposerMode();
        });
    }
    static QString effectiveWork(const QJsonObject& work)
    {
        QString state = text(work, "effectiveState", text(work, "state"));
        if (state == QStringLiteral("active") && number(work, "activeUntil", 0) <= QDateTime::currentMSecsSinceEpoch())
            return QStringLiteral("unconfirmed");
        if (state == QStringLiteral("active") || state == QStringLiteral("waiting") || state == QStringLiteral("blocked")
            || state == QStringLiteral("completed") || state == QStringLiteral("cancelled") || state == QStringLiteral("unconfirmed")) return state;
        return QStringLiteral("unconfirmed");
    }
    static QString workTooltip(const QJsonObject& work)
    {
        const QString state = effectiveWork(work);
        QString words = state == QStringLiteral("active") ? QStringLiteral("DJ radi na ovome.")
            : state == QStringLiteral("waiting") ? QStringLiteral("Čeka sljedeći korak.")
            : state == QStringLiteral("blocked") ? QStringLiteral("Rad je blokiran.")
            : state == QStringLiteral("completed") ? QStringLiteral("Rad je dovršen.")
            : state == QStringLiteral("cancelled") ? QStringLiteral("Rad je otkazan.")
            : QStringLiteral("Aktivni rad nije ponovno potvrđen.");
        for (const auto& field : {QStringLiteral("summary"), QStringLiteral("outcome"), QStringLiteral("evidence")}) {
            const auto value = work.value(field); QString detail;
            if (value.isString()) detail = value.toString();
            else if (value.isArray()) detail = QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
            else if (value.isObject()) detail = QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
            if (!detail.isEmpty()) words += QChar('\n') + detail.left(800);
        }
        return QStringLiteral("<qt>") + words.toHtmlEscaped().replace(QChar('\n'), QStringLiteral("<br>")) + QStringLiteral("</qt>");
    }
    bool mergeWork(const QJsonArray& incoming, const QJsonObject& updates)
    {
        auto next = messageWork_;
        const auto remember = [&](const QString& id, const QJsonObject& work) {
            if (id.isEmpty()) return;
            const auto previous = next.value(id).toObject();
            if (!previous.isEmpty() && number(work, "updatedAt", 0) < number(previous, "updatedAt", 0)) return;
            next.insert(id, work);
        };
        for (const auto& value : incoming) {
            const auto message = value.toObject();
            if (message.value(QStringLiteral("work")).isObject())
                remember(message.value(QStringLiteral("id")).toVariant().toString(), message.value(QStringLiteral("work")).toObject());
        }
        for (auto it = updates.begin(); it != updates.end(); ++it) if (it.value().isObject()) remember(it.key(), it.value().toObject());
        if (next == messageWork_) return false;
        messageWork_ = next; return true;
    }
    QJsonObject workStates() const
    {
        QJsonObject states;
        for (auto it = messageWork_.begin(); it != messageWork_.end(); ++it) states.insert(it.key(), effectiveWork(it.value().toObject()));
        return states;
    }
    int activeWorkCount() const
    {
        QSet<QString> active;
        for (const auto& id : processingIds_) if (!messageWork_.contains(id)) active.insert(id);
        for (auto it = messageWork_.begin(); it != messageWork_.end(); ++it)
            if (effectiveWork(it.value().toObject()) == QStringLiteral("active")) active.insert(it.key());
        return active.size();
    }
    void refreshWorkBadges()
    {
        if (workStates() == workPresentation_) return;
        appendMessages({}, true);
        if (activeWorkCount() == 0 && presence_->text() == QStringLiteral("DJ radi na poruci…"))
            presence_->setText(QStringLiteral("Razgovor s DJ-em"));
    }
    QNetworkAccessManager network_{this};
    QTimer searchDelay_, chatPoll_, idleReturn_, descriptionPoll_, noticeDismiss_;
    QElapsedTimer idleClock_;
    QPropertyAnimation* chatScroll_;
    QUrl base_;
    QUrl activeSearchBase_;
    QString activeSearchId_;
    QPointer<QNetworkReply> activeSearchReply_;
    QLineEdit* name_;
    QPlainTextEdit* message_;
    QListWidget* results_;
    QStackedWidget* suggestions_;
    QProgressBar* searchActivity_;
    QWidget *selectedPage_, *suggestionArea_;
    QPushButton *send_, *cancelSong_, *describe_, *alternate_;
    QLabel *presence_, *notice_, *suggestionHint_, *selectedTitle_, *selectedArtist_, *magicHelp_, *onlineBadge_ = nullptr;
    QTextBrowser* chat_;
    QNetworkAccessManager resultArtNetwork_{this};
    QCache<QString, QImage> resultArtCache_{24};
    QHash<QString, QPointer<QNetworkReply>> resultArtReplies_;
    QQueue<QString> resultArtQueue_;
    quint64 resultArtVersion_ = 0;
    QString guestId_, composerId_, searchOverride_, cursor_, requestId_, lastRequestSignature_, messageId_, lastMessage_;
    QString savedQuery_, suggestionState_;
    QString descriptionId_, descriptionText_, descriptionRequestId_, descriptionStatus_;
    QString alternateLabel_ = QStringLiteral("Vrati skicu");
    QJsonObject alternateDraft_, messageWork_, workPresentation_;
    QJsonObject observedPresence_;
    QElapsedTimer presenceClock_;
    qint64 presenceDeadline_ = 0;
    double presenceExpiryWatermark_ = 0;
    qint64 presenceWatermarkDeadline_ = 0;
    int presenceActiveCount_ = 0;
    bool presenceKnown_ = false, presenceSeen_ = false;
    qint64 lastSentId_ = 0;
    int savedQueryCursor_ = 0, savedQueryAnchor_ = 0;
    QJsonObject selectedTrack_;
    QJsonArray messages_;
    QSet<QString> messageIds_, processingIds_, withdrawnIds_;
    quint64 searchVersion_ = 0, composerGeneration_ = 0, draftRevision_ = 0;
    quint64 chatFetches_ = 0, serviceGeneration_ = 0;
    bool chatBusy_ = false, requestBusy_ = false, messageBusy_ = false, followingBottom_ = false;
    bool recallBusy_ = false, descriptionActive_ = false, descriptionBusy_ = false;
};

class PartyStage final : public QWidget {
public:
    explicit PartyStage(CubeWidget* cube, const QString& stateFile, const QString& requestBaseUrl = {})
        : QWidget(cube), cube_(cube), originalTitle_(cube->window()->windowTitle())
    {
        setObjectName(QStringLiteral("partyStage"));
        if (!requestBaseUrl.isEmpty()) setProperty("requestBaseUrl", requestBaseUrl);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);
        setGeometry(cube->rect());
        cube->installEventFilter(this);
        clock_.start();
        frame_.setTimerType(Qt::PreciseTimer);
        frame_.setInterval(33);
        connect(&frame_, &QTimer::timeout, this, [this] { tick(); });
        poll_.setInterval(250);
        connect(&poll_, &QTimer::timeout, this, [this] { readState(); updatePlaybackControl(); });
        poll_.start();
        auto* window = cube->window();
        originalCursor_ = window->cursor(); cursorWasSet_ = window->testAttribute(Qt::WA_SetCursor);
        if (window->windowHandle()) originalNativeCursor_ = window->windowHandle()->cursor();
        window->setCursor(djCursor());
        // A disabled child uses the input surface's cursor, not its own cached
        // bitmap. Give the GL surface an explicit cursor as well as the window.
        originalCubeCursor_ = cube_->cursor(); cubeCursorWasSet_ = cube_->testAttribute(Qt::WA_SetCursor);
        cube_->setCursor(djCursor());
        playbackButton_ = new QPushButton(cube_); playbackButton_->setObjectName(QStringLiteral("partyPlaybackButton"));
        playbackButton_->setFocusPolicy(Qt::NoFocus); playbackButton_->setCursor(djCursor());
        playbackButton_->installEventFilter(this);
        connect(playbackButton_, &QPushButton::clicked, this, [this] { setPlaybackPaused(); });
        playbackStatus_ = new QLabel(cube_); playbackStatus_->setObjectName(QStringLiteral("partyPlaybackStatus"));
        playbackStatus_->setAttribute(Qt::WA_TransparentForMouseEvents);
        playbackStatus_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        playbackAction_ = createShortcut(GuestShortcut::Playback, [this] { setPlaybackPaused(); });
        previousAction_ = createShortcut(GuestShortcut::Previous, [this] { navigatePlayback(QStringLiteral("previous")); });
        nextAction_ = createShortcut(GuestShortcut::Next, [this] { navigatePlayback(QStringLiteral("next")); });
        lyricsMode_ = window->property("partyLyricsMode").toBool();
        lyricsAction_ = createShortcut(GuestShortcut::Lyrics, [this] { setLyricsMode(!lyricsMode_); });
        lyricsAction_->setCheckable(true); lyricsAction_->setChecked(lyricsMode_);
        if (auto* main = qobject_cast<QMainWindow*>(window)) {
            menuWasVisible_ = main->menuBar()->isVisible(); statusWasVisible_ = main->statusBar()->isVisible();
            main->menuBar()->hide(); main->statusBar()->hide();
        }
        requestAction_ = createShortcut(GuestShortcut::Search, [this] { openRequests(); });
        fullAction_ = new QAction(QStringLiteral("Party fullscreen"), this);
        fullAction_->setObjectName(QStringLiteral("partyFullscreenAction"));
        fullAction_->setShortcut(QKeySequence(QStringLiteral("F11")));
        window->addAction(fullAction_);
        connect(fullAction_, &QAction::triggered, this, [this] {
            QWidget* w = cube_->window();
            w->isFullScreen() ? w->showNormal() : w->showFullScreen();
        });
        escapeAction_ = new QAction(this);
        escapeAction_->setObjectName(QStringLiteral("partyRecallAction")); escapeAction_->setAutoRepeat(false);
        escapeAction_->setShortcut(QKeySequence(Qt::Key_Escape));
        window->addAction(escapeAction_);
        connect(escapeAction_, &QAction::triggered, this, [this] { if (desk_) desk_->recallLatest(); });
        spotifyAction_ = createShortcut(GuestShortcut::Spotify, [this] { showSpotify(); });
        returnAction_ = new QAction(this); returnAction_->setObjectName(QStringLiteral("partyReturnAction"));
        window->addAction(returnAction_);
        connect(returnAction_, &QAction::triggered, this, [this] {
            auto* window = cube_->window(); window->showFullScreen(); window->raise(); window->activateWindow();
            QTimer::singleShot(0, this, [this] {
                if (returnFocus_) returnFocus_->setFocus();
                else if (desk_) if (auto* composer = desk_->findChild<QPlainTextEdit*>(QStringLiteral("partyMessage"))) composer->setFocus();
            });
        });
        for (std::size_t i = 0; i < guestShortcuts.size(); ++i) {
            auto* action = shortcutAction(static_cast<GuestShortcut>(i));
            auto* button = new QPushButton(cube_); shortcutButtons_[i] = button;
            button->setObjectName(QString::fromUtf8(guestShortcuts[i].objectName).replace(QStringLiteral("Action"), QStringLiteral("ShortcutButton")));
            button->setFocusPolicy(Qt::NoFocus); button->setCursor(djCursor()); button->installEventFilter(this);
            connect(button, &QPushButton::clicked, action, &QAction::trigger);
        }
        configureWindowToggle(QJsonObject::fromVariantMap(window->property("partyWindowToggleConfig").toMap()));
        window->setWindowTitle(QStringLiteral("LARICS / LYRICS · after hours"));
        setStateFile(stateFile);
        showRequests(true);
        show(); raise();
    }
    ~PartyStage() override
    {
        for (auto* manager : {&artworkNetwork_, &controlNetwork_})
            for (auto* reply : manager->findChildren<QNetworkReply*>()) { disconnect(reply, nullptr, this, nullptr); reply->abort(); }
        delete playbackButton_;
        delete playbackStatus_;
        for (const auto& button : shortcutButtons_) delete button.data();
        delete desk_.data();
        if (cube_) {
            for (auto it = effectBefore_.begin(); it != effectBefore_.end(); ++it) {
                if (auto* action = cube_->window()->findChild<QAction*>(it.key())) {
                    if (action->isChecked() != it.value()) action->trigger();
                }
            }
            cube_->removeEventFilter(this);
            cube_->window()->setWindowTitle(originalTitle_);
            if (cubeCursorWasSet_) cube_->setCursor(originalCubeCursor_);
            else cube_->unsetCursor();
            if (cursorWasSet_) cube_->window()->setCursor(originalCursor_);
            else cube_->window()->unsetCursor();
            if (cube_->window()->windowHandle()) cube_->window()->windowHandle()->setCursor(originalNativeCursor_);
            if (auto* main = qobject_cast<QMainWindow*>(cube_->window())) {
                main->menuBar()->setVisible(menuWasVisible_); main->statusBar()->setVisible(statusWasVisible_);
            }
            for (QAction* a : {requestAction_, fullAction_, escapeAction_, spotifyAction_, returnAction_, playbackAction_, previousAction_, nextAction_, lyricsAction_}) cube_->window()->removeAction(a);
        }
    }
    void setStateFile(const QString& file) { stateFile_ = file; bytes_.clear(); readState(); }
    void readState()
    {
        QFile file(stateFile_);
        if (!file.open(QIODevice::ReadOnly)) { fileError_ = file.errorString(); return; }
        const QByteArray data = file.read(1024 * 1024 + 1);
        if (data.size() > 1024 * 1024) { fileError_ = QStringLiteral("State exceeds 1 MiB"); return; }
        if (!data.isEmpty() && data == bytes_) { fileError_.clear(); return; }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(data, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            fileError_ = QStringLiteral("State JSON: ") + error.errorString(); return;
        }
        bytes_ = data; fileError_.clear(); applyState(document.object());
    }
    void applyState(const QJsonObject& state)
    {
        state_ = state;
        const auto caption = state.value(QStringLiteral("caption")).toObject();
        const QString id = text(caption, "id");
        const QString words = text(caption, "text");
        if (!words.isEmpty() && !id.isEmpty() && !seenCues_.contains(id)) {
            seenCues_.insert(id);
            const qint64 created = static_cast<qint64>(number(caption, "createdAt", QDateTime::currentMSecsSinceEpoch()));
            // An old state file can survive a restart. A caption is a live cue,
            // so a cue from more than three minutes ago is already finished.
            if (QDateTime::currentMSecsSinceEpoch() - created < 180000) {
                Cue cue{id, words, std::max(number(caption, "seconds", 7), words.size() / 17.0 + 1.8), created};
                cue.seconds = std::max(cue.seconds, 4.5);
                pending_.enqueue(cue); lastExternalCue_ = clock_.elapsed() / 1000.0;
            }
        }
        const QJsonObject now = state.value(QStringLiteral("nowPlaying")).toObject();
        updateTrackClock(now);
        loadArtwork(now);
        const QString track = text(now, "uri", text(now, "id", text(now, "title") + text(now, "artist")));
        if (!text(now, "title").isEmpty() && track != lastTrack_) {
            lastTrack_ = track;
            missionStart_ = seconds(); missionKind_ = (missionKind_ + 1) % 4;
            missionText_ = QStringLiteral("Posebna dostava: ") + text(now, "title");
        }
        const auto queue = state.value(QStringLiteral("queue")).toArray();
        if (!queue.isEmpty()) {
            const auto last = queue.last().toObject();
            const QString key = text(last, "id", text(last, "uri", text(last, "title") + text(last, "requestedBy")));
            if (key != lastRequest_) {
                if (seconds() - missionStart_ > 10) {
                    missionStart_ = seconds(); missionKind_ = (missionKind_ + 1) % 4;
                    missionText_ = text(last, "requestedBy", QStringLiteral("Ekipa"))
                        + QStringLiteral(" šalje: ") + text(last, "title");
                }
                lastRequest_ = key;
            }
        }
        loadImage(text(state, "qrPath"), qrPath_, qr_, QSize(320, 320));
        const auto book = state.value(QStringLiteral("bookMoment")).toObject();
        loadImage(text(book, "imagePath"), bookPath_, book_, QSize(2200, 1600));
        if (desk_) desk_->setBase(requestUrl());
        setProperty("revision", state.value(QStringLiteral("revision")).toVariant());
        setProperty("scene", sceneMode());
        setProperty("graphicsScale", graphicsScale());
        setProperty("trackTitle", text(now, "title"));
        applyEffect();
        updatePlaybackControl();
        update();
    }
    QJsonObject status() const
    {
        QJsonObject result{{QStringLiteral("active"), true}, {QStringLiteral("stateFile"), stateFile_},
                {QStringLiteral("revision"), state_.value(QStringLiteral("revision"))},
                {QStringLiteral("scene"), sceneMode()}, {QStringLiteral("graphicsScale"), graphicsScale()},
                {QStringLiteral("captionId"), cue_.id},
                {QStringLiteral("caption"), cue_.text}, {QStringLiteral("captionSeconds"), cue_.seconds},
                {QStringLiteral("pendingCaptions"), pending_.size()},
                {QStringLiteral("effect"), effectIndex_}, {QStringLiteral("effectError"), effectError_},
                {QStringLiteral("fileError"), fileError_}, {QStringLiteral("paintCount"), QString::number(paints_)},
                {QStringLiteral("requestUrl"), requestUrl()},
                {QStringLiteral("djCursorPixels"), cube_->window()->cursor().pixmap().width()},
                {QStringLiteral("windowToggleConfigured"), spotifyAction_->isEnabled()},
                {QStringLiteral("windowToggleBusy"), !toggleProcess_.isNull()}, {QStringLiteral("windowToggleError"), windowToggleError_},
                {QStringLiteral("trackTitle"), text(state_.value(QStringLiteral("nowPlaying")).toObject(), "title")},
                {QStringLiteral("bookLoaded"), !book_.isNull()}, {QStringLiteral("qrLoaded"), !qr_.isNull()}};
        result.insert(QStringLiteral("artwork"), QJsonObject{{QStringLiteral("loaded"), !artwork_.isNull()},
            {QStringLiteral("url"), artworkUrl_}, {QStringLiteral("error"), artworkError_},
            {QStringLiteral("cacheSize"), artworkCache_.size()}, {QStringLiteral("busy"), !artworkReply_.isNull()}});
        const auto control = playbackView();
        const QRect r = playbackButton_->geometry();
        result.insert(QStringLiteral("playbackControl"), QJsonObject{{QStringLiteral("enabled"), control.enabled},
            {QStringLiteral("paused"), control.paused}, {QStringLiteral("known"), control.known},
            {QStringLiteral("busy"), playbackBusy_}, {QStringLiteral("reason"), control.reason},
            {QStringLiteral("label"), playbackButton_->text()}, {QStringLiteral("verification"), playbackStatus_->text()},
            {QStringLiteral("cursorPixels"), playbackButton_->cursor().pixmap().width()},
            {QStringLiteral("bounds"), QJsonArray{r.x(), r.y(), r.width(), r.height()}}});
        QJsonArray shortcutStatus;
        for (std::size_t i = 0; i < guestShortcuts.size(); ++i) {
            const auto kind = static_cast<GuestShortcut>(i); const auto* action = shortcutAction(kind);
            const auto* button = shortcutButtons_[i].data(); const QRect bounds = button ? button->geometry() : QRect();
            shortcutStatus.append(QJsonObject{{QStringLiteral("action"), action->objectName()},
                {QStringLiteral("key"), action->shortcut().toString(QKeySequence::NativeText)},
                {QStringLiteral("label"), button ? button->text() : QString()}, {QStringLiteral("enabled"), action->isEnabled()},
                {QStringLiteral("checked"), action->isChecked()},
                {QStringLiteral("autoRepeat"), action->autoRepeat()},
                {QStringLiteral("bounds"), QJsonArray{bounds.x(), bounds.y(), bounds.width(), bounds.height()}}});
        }
        result.insert(QStringLiteral("shortcutControls"), shortcutStatus);
        result.insert(QStringLiteral("queuePreview"), queuePreview_);
        const auto bounds = [](const QRectF& r) { return QJsonArray{r.x(), r.y(), r.width(), r.height()}; };
        result.insert(QStringLiteral("requestCardBounds"), bounds(requestRect_));
        result.insert(QStringLiteral("qrBounds"), bounds(qrBounds_));
        result.insert(QStringLiteral("playingCardBounds"), bounds(playingBounds_));
        result.insert(QStringLiteral("trackProgressMs"), trackProgress());
        result.insert(QStringLiteral("trackClockValid"), trackClockKnown_ && trackClockTrusted_
            && (!trackClockPlaying_ || QDateTime::currentMSecsSinceEpoch() <= trackClockStamp_ + 10000));
        result.insert(QStringLiteral("panelVisible"), false);
        if (desk_) {
            const auto values = desk_->status();
            for (auto it = values.begin(); it != values.end(); ++it) result.insert(it.key(), it.value());
        }
        const auto karaoke = karaokeFrame();
        QJsonObject singing{{QStringLiteral("active"), karaoke.visible && karaoke.available}, {QStringLiteral("mode"), lyricsMode_},
            {QStringLiteral("panelVisible"), karaoke.visible}, {QStringLiteral("available"), karaoke.available},
            {QStringLiteral("manual"), karaoke.manual}, {QStringLiteral("title"), karaoke.title}, {QStringLiteral("reason"), karaoke.reason},
            {QStringLiteral("positionMs"), karaoke.position},
            {QStringLiteral("status"), karaoke.status}, {QStringLiteral("phase"), karaoke.phase},
            {QStringLiteral("clockState"), karaoke.clockState},
            {QStringLiteral("countInMs"), karaoke.countIn},
            {QStringLiteral("lineIndex"), karaoke.lineIndex}, {QStringLiteral("syllableIndex"), karaoke.syllableIndex},
            {QStringLiteral("text"), text(karaoke.line, "text")}, {QStringLiteral("nextText"), text(karaoke.next, "text")}};
        if (karaoke.visible) for (auto it = karaokeGeometry_.begin(); it != karaokeGeometry_.end(); ++it) singing.insert(it.key(), it.value());
        result.insert(QStringLiteral("karaoke"), singing);
        return result;
    }
    QString requestUrl() const
    {
        const QString override = property("requestBaseUrl").toString();
        if (!override.isEmpty()) return override;
        return text(state_, "requestLocalUrl", QStringLiteral("http://127.0.0.1:8787"));
    }
    void setRequestBaseUrl(const QString& url)
    {
        setProperty("requestBaseUrl", url);
        if (desk_) desk_->setBase(requestUrl());
    }
    void setLyricsMode(bool enabled)
    {
        lyricsMode_ = enabled;
        cube_->window()->setProperty("partyLyricsMode", enabled);
        lyricsAction_->setChecked(enabled);
        karaokeGeometry_ = {};
        updatePlaybackControl(); update();
    }
    void showRequests(bool visible)
    {
        Q_UNUSED(visible);
        if (!desk_) {
            auto* window = qobject_cast<QMainWindow*>(cube_->window());
            if (!window) { QDesktopServices::openUrl(QUrl(requestUrl())); return; }
            desk_ = new RequestDesk(window, requestUrl());
        }
        desk_->show();
    }
    QJsonObject composerDraft() const { return desk_ ? desk_->draft() : QJsonObject(); }
    void restoreComposerDraft(const QJsonObject& draft)
    {
        if (!desk_) showRequests(true);
        if (desk_) desk_->restoreDraft(draft);
    }
    QJsonObject setSearchIntent(const QJsonObject& cue)
    {
        return desk_ ? desk_->setSearchIntent(cue) : QJsonObject{{QStringLiteral("applied"), false}, {QStringLiteral("error"), QStringLiteral("Request desk is unavailable.")}};
    }
    void configureWindowToggle(const QJsonObject& config)
    {
        windowToggle_ = config;
        cube_->window()->setProperty("partyWindowToggleConfig", config.toVariantMap());
        spotifyAction_->setEnabled(QFileInfo::exists(text(config, "script")) && !text(config, "socket").isEmpty()
                                  && !text(config, "data").isEmpty());
    }
protected:
    void showEvent(QShowEvent* e) override { frame_.start(); QWidget::showEvent(e); updatePlaybackControl(); }
    void hideEvent(QHideEvent* e) override {
        frame_.stop(); if (playbackButton_) playbackButton_->hide(); if (playbackStatus_) playbackStatus_->hide();
        for (const auto& button : shortcutButtons_) if (button) button->hide();
        QWidget::hideEvent(e);
    }
    bool eventFilter(QObject* o, QEvent* e) override
    {
        const bool controlEvent = o == playbackButton_ || std::any_of(shortcutButtons_.begin(), shortcutButtons_.end(), [o](const auto& button) { return o == button.data(); });
        if (controlEvent && (e->type() == QEvent::EnabledChange || e->type() == QEvent::MouseButtonRelease
                                    || e->type() == QEvent::Enter || e->type() == QEvent::StyleChange)) {
            if (!cursorRefreshQueued_) {
                cursorRefreshQueued_ = true;
                QTimer::singleShot(0, this, [this] {
                    cursorRefreshQueued_ = false;
                    if (!cube_) return;
                    const QPoint pointer = QCursor::pos();
                    const auto underPointer = [&pointer](const auto& button) {
                        return button && button->isVisible() && button->rect().contains(button->mapFromGlobal(pointer));
                    };
                    if (!underPointer(playbackButton_) && !std::any_of(shortcutButtons_.begin(), shortcutButtons_.end(), underPointer)) return;
                    auto* hovered = QApplication::widgetAt(pointer);
                    if (!hovered || hovered->window() != cube_->window()) return;
                    // Qt may apply a system cursor when disabling the child,
                    // without changing its cached QCursor. Refresh the actual
                    // native surface after that transition has finished.
                    if (auto* native = cube_->window()->windowHandle()) native->setCursor(djCursor());
                });
            }
        }
        if (o == cube_) {
            if (e->type() == QEvent::Resize) { setGeometry(cube_->rect()); raise(); updatePlaybackControl(); }
            if (e->type() == QEvent::MouseButtonRelease) {
                auto* mouse = static_cast<QMouseEvent*>(e);
                if (mouse->button() == Qt::LeftButton && requestRect_.contains(mouse->position())) {
                    openRequests(); return true;
                }
            }
        }
        return QWidget::eventFilter(o, e);
    }
    void paintEvent(QPaintEvent*) override
    {
        ++paints_;
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        const double w = width(), h = height(), t = seconds();
        const double u = std::clamp(std::min(w / 1280.0, h / 800.0), .65, 2.0);
        const QColor orange = palette() == QStringLiteral("ice") ? QColor("#7bdffc") : QColor("#ff8a42");
        const QColor cyan("#7de4ed"), lilac("#a79bf5"), white("#f7f2ea");
        const QString mode = sceneMode();
        const double energy = unit(number(state_.value(QStringLiteral("scene")).toObject(), "energy", .55));
        // The host's audio snippet owns scale and tint. We observe those outputs
        // instead of opening a second audio stream.
        const double pulse = cube_ ? unit((cube_->m_tint.y() - .5) * 2.2) : 0;
        const double reaction = .20 + energy * .8;
        const double margin = 34 * u;
        {
            QLinearGradient top(0, 0, 0, h * .34);
            top.setColorAt(0, QColor(6, 10, 22, 235)); top.setColorAt(1, QColor(6, 10, 22, 0));
            p.fillRect(QRectF(0, 0, w, h * .34), top);
            QLinearGradient bottom(0, h * .62, 0, h);
            bottom.setColorAt(0, QColor(6, 10, 22, 0)); bottom.setColorAt(1, QColor(6, 10, 22, 235));
            p.fillRect(QRectF(0, h * .62, w, h * .38), bottom);

            if (mode == QStringLiteral("laser")) {
                p.save(); p.setCompositionMode(QPainter::CompositionMode_Plus);
                for (int i = 0; i < 12; ++i) {
                    const double a = t * .11 + i * pi / 6;
                    const QPointF start(w * (i % 2 ? .92 : .08), h * .78);
                    const QPointF end(w * .5 + std::sin(a) * w * .62, h * (.19 + .18 * std::cos(a * 1.3)));
                    line(p, start, end, alpha(i % 2 ? cyan : orange, (.05 + pulse * .10) * reaction), 2 * u);
                }
                p.restore();
            } else if (mode != QStringLiteral("quiet")) {
                p.save(); p.setCompositionMode(QPainter::CompositionMode_Plus);
                for (int i = 0; i < 38; ++i) {
                    const double a = i * 2.399963 + t * (.04 + (i % 4) * .009);
                    const double r = .20 + .26 * ((i * 17) % 37) / 37.0;
                    const QPointF v(w * .5 + std::cos(a) * w * r, h * .49 + std::sin(a) * h * r * .7);
                    p.setPen(Qt::NoPen); p.setBrush(alpha(i % 3 ? cyan : orange, .18 + pulse * .22));
                    p.drawEllipse(v, (1.0 + i % 3 + pulse) * u, (1.0 + i % 3 + pulse) * u);
                }
                p.restore();
            }

            p.setFont(uiFont(u, UiMetrics::body, true)); p.setPen(orange);
            p.drawText(QRectF(margin, 22 * u, w * .55, 28 * u), Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("LARICS / LYRICS"));
            p.setFont(uiFont(u, UiMetrics::small)); p.setPen(QColor("#b7bfd1"));
            p.drawText(QRectF(margin, 49 * u, w * .55, 21 * u), Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("after hours   ·   robots have requests too"));

            const auto now = state_.value(QStringLiteral("nowPlaying")).toObject();
            const QString title = text(now, "title", QStringLiteral("The lab is warming up"));
            const QString artist = text(now, "artist", QStringLiteral("Odaberi pjesmu. Roboti čekaju."));
            const double infoWidth = std::min(w * .53, 690 * u);
            const QRectF playing(margin, 89 * u, infoWidth, 105 * u);
            playingBounds_ = playing;
            panel(p, playing, orange);
            const QRectF cover(playing.x() + 10 * u, playing.y() + 11 * u, 82 * u, 82 * u);
            p.setPen(QPen(alpha(orange, .35), 1)); p.setBrush(QColor("#142135")); p.drawRoundedRect(cover, 7 * u, 7 * u);
            if (!artwork_.isNull()) {
                const QSizeF fitted = QSizeF(artwork_.size()).scaled(cover.size(), Qt::KeepAspectRatio);
                const QRectF image(cover.center() - QPointF(fitted.width() / 2, fitted.height() / 2), fitted);
                p.setRenderHint(QPainter::SmoothPixmapTransform); p.drawImage(image, artwork_);
            } else {
                p.setPen(alpha(orange, .8)); p.setFont(stageFont(37 * u)); p.drawText(cover, Qt::AlignCenter, QStringLiteral("♪"));
            }
            const double textInset = 106 * u;
            p.setPen(orange); p.setFont(uiFont(u, UiMetrics::body, true));
            p.drawText(playing.adjusted(textInset, 12 * u, -19 * u, 0), Qt::AlignTop,
                       now.value(QStringLiteral("playing")).toBool() ? QStringLiteral("Sada svira") : QStringLiteral("Na repertoaru"));
            p.setPen(white); p.setFont(stageFont(25 * u, true));
            p.drawText(playing.adjusted(textInset, 33 * u, -19 * u, -30 * u), Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetrics(p.font()).elidedText(title, Qt::ElideRight, qRound(infoWidth - textInset - 19 * u)));
            p.setPen(QColor("#b9c7db")); p.setFont(uiFont(u, UiMetrics::secondary));
            p.drawText(playing.adjusted(textInset, 71 * u, -19 * u, 0), Qt::AlignTop,
                       QFontMetrics(p.font()).elidedText(artist, Qt::ElideRight, qRound(infoWidth - textInset - 19 * u)));
            const double duration = number(now, "durationMs", 0);
            if (duration > 0 && trackClockKnown_) {
                const double progress = unit(trackProgress() / duration);
                p.setPen(Qt::NoPen); p.setBrush(alpha(orange, .14)); p.drawRoundedRect(QRectF(playing.x() + 17 * u, playing.bottom() - 5 * u, infoWidth - 34 * u, 3 * u), 1.5, 1.5);
                p.setBrush(orange); p.drawRoundedRect(QRectF(playing.x() + 17 * u, playing.bottom() - 5 * u, (infoWidth - 34 * u) * progress, 3 * u), 1.5, 1.5);
            }

            const double requestWidth = requestCardWidth(w, u);
            requestRect_ = QRectF(w - margin - requestWidth, playing.y(), requestWidth, 180 * u);
            const bool wideQr = requestWidth >= 300 * u;
            const double qrSide = std::min((wideQr ? 152 : 116) * u, requestWidth - 28 * u);
            panel(p, requestRect_, cyan);
            p.setFont(uiFont(u, UiMetrics::body, true)); p.setPen(white);
            const QRectF requestHeading(requestRect_.x() + 16 * u, requestRect_.y() + 12 * u,
                                        requestWidth - (wideQr ? qrSide + 48 * u : 32 * u), 27 * u);
            p.drawText(requestHeading, (wideQr ? Qt::AlignLeft : Qt::AlignHCenter) | Qt::AlignTop,
                       QFontMetrics(p.font()).elidedText(QStringLiteral("Daj pjesmu."), Qt::ElideRight, qRound(requestHeading.width())));
            const QRectF qrRect(wideQr ? requestRect_.right() - qrSide - 14 * u : requestRect_.center().x() - qrSide / 2,
                                requestRect_.y() + (wideQr ? 14 : 42) * u, qrSide, qrSide);
            qrBounds_ = qrRect;
            if (!qr_.isNull()) {
                p.fillRect(qrRect.adjusted(-4 * u, -4 * u, 4 * u, 4 * u), Qt::white);
                p.setRenderHint(QPainter::SmoothPixmapTransform, false);
                p.drawImage(qrRect, qr_);
            } else {
                p.setPen(alpha(cyan, .45)); p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(qrRect, 12, 12);
                p.setFont(stageFont(28 * u, true)); p.setPen(cyan);
                p.drawText(qrRect, Qt::AlignCenter, QStringLiteral("♪"));
            }
            p.setFont(uiFont(u, UiMetrics::secondary, true)); p.setPen(cyan);
            const double labelWidth = wideQr ? requestHeading.width() : requestWidth - 16 * u;
            if (wideQr) {
                p.setFont(uiFont(u, UiMetrics::secondary)); p.setPen(QColor("#c1d1e5"));
                p.drawText(QRectF(requestHeading.x(), requestRect_.y() + 49 * u, labelWidth, 61 * u), Qt::AlignLeft | Qt::TextWordWrap,
                           QStringLiteral("Skeniraj i pošalji pjesmu DJ-u."));
            }
            p.setFont(uiFont(u, UiMetrics::secondary, true)); p.setPen(cyan);
            p.drawText(QRectF(requestRect_.x() + (wideQr ? 16 : 8) * u, requestRect_.y() + (wideQr ? 123 : 158) * u, labelWidth, 24 * u),
                       wideQr ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignCenter,
                       shortcutKey(GuestShortcut::Search) + (wideQr ? QStringLiteral(" · zatraži") : QString()));
            const QString guestUrl = text(state_, "requestUrl");
            if (!guestUrl.isEmpty() && wideQr) {
                p.setFont(uiFont(u, UiMetrics::small)); p.setPen(QColor("#b2c5df"));
                p.drawText(QRectF(requestHeading.x(), requestRect_.y() + 151 * u, labelWidth, 22 * u), Qt::AlignLeft | Qt::AlignVCenter,
                           QFontMetrics(p.font()).elidedText(guestUrl, Qt::ElideMiddle, qRound(labelWidth)));
            }

            const double mission = t - missionStart_;
            if (mission >= 0 && mission < 17) drawDelivery(p, w, h, u, t, pulse, orange, mission);
            if (mode == QStringLiteral("robots")) {
                for (int i = 0; i < 3; ++i) {
                    double robotScale = (i == 0 ? 1.05 : 1.15) * u * graphicsScale();
                    double x = w * (.20 + i * .30) + std::sin(t * .5 + i) * 24 * u;
                    double y = h * (.68 + (i == 0 ? -.07 : .03));
                    y = std::min(y, graphicsBottom(w, h, u) - 42 * robotScale - 36);
                    robot(p, i, QPointF(x, y), robotScale, t + i, t * 18, std::cos(t + i), pulse, false);
                }
            }
        }
        drawBook(p, w, h, u, orange);
        drawKaraoke(p, w, h, u, orange);
        drawCaption(p, w, h, u, white, orange);

        queuePreview_ = {};
        const auto queue = state_.value(QStringLiteral("queue")).toArray();
        if (!queue.isEmpty()) {
            const int count = std::min(int(queue.size()), std::clamp(int(w / (250 * u)), 1, 4));
            const double gap = 8 * u, cell = (w - 2 * margin - gap * (count - 1)) / count;
            for (int i = 0; i < count; ++i) {
                const auto track = queue[i].toObject();
                const QRectF box(margin + i * (cell + gap), h - 70 * u, cell, 62 * u);
                panel(p, box, orange, .92);
                p.setFont(uiFont(u, UiMetrics::icon, true)); p.setPen(orange);
                p.drawText(QRectF(box.x() + 8 * u, box.y(), 25 * u, box.height()), Qt::AlignVCenter, QString::number(i + 1));
                p.setFont(uiFont(u, UiMetrics::body, true)); p.setPen(white);
                p.drawText(QRectF(box.x() + 38 * u, box.y() + 5 * u, cell - 46 * u, 28 * u), Qt::AlignVCenter,
                    QFontMetrics(p.font()).elidedText(text(track, "title"), Qt::ElideRight, qRound(cell - 46 * u)));
                const QString detail = text(track, "status") == QStringLiteral("pending") ? QStringLiteral("Zahtjev čeka DJ-a") : text(track, "artist");
                p.setFont(uiFont(u, UiMetrics::secondary)); p.setPen(QColor("#c3d1e3"));
                p.drawText(QRectF(box.x() + 38 * u, box.y() + 34 * u, cell - 46 * u, 23 * u), Qt::AlignVCenter,
                    QFontMetrics(p.font()).elidedText(detail, Qt::ElideRight, qRound(cell - 46 * u)));
                queuePreview_.append(QJsonObject{{QStringLiteral("title"), text(track, "title")},
                    {QStringLiteral("titleFontPixels"), uiFont(u, UiMetrics::body, true).pixelSize()},
                    {QStringLiteral("detailFontPixels"), p.font().pixelSize()},
                    {QStringLiteral("uid"), text(track, "uid")}, {QStringLiteral("bounds"), QJsonArray{box.x(), box.y(), box.width(), box.height()}}});
            }
            return;
        }
        p.setFont(uiFont(u, UiMetrics::secondary)); p.setPen(QColor("#a3b3c9"));
        const auto mood = state_.value(QStringLiteral("mood")).toObject();
        QString moodLabel = text(mood, "label", QStringLiteral("Dobra frekvencija"));
        QString next;
        if (!queue.isEmpty()) {
            const auto q = queue.first().toObject();
            next = QStringLiteral("Slijedi  ") + text(q, "title");
            if (!text(q, "requestedBy").isEmpty()) next += QStringLiteral("  ·  ") + text(q, "requestedBy");
        } else next = QStringLiteral("Tvoj zahtjev može biti sljedeći.");
        p.drawText(QRectF(margin, h - 34 * u, w * .68, 24 * u), Qt::AlignVCenter,
                   QFontMetrics(p.font()).elidedText(next, Qt::ElideRight, qRound(w * .68)));
        p.setPen(orange);
        p.drawText(QRectF(w * .71, h - 34 * u, w * .29 - margin, 24 * u), Qt::AlignRight | Qt::AlignVCenter,
                   moodLabel + QStringLiteral("  ·  F11"));
    }
private:
    QAction* createShortcut(GuestShortcut kind, std::function<void()> triggered)
    {
        const auto& spec = shortcutSpec(kind);
        auto* action = new QAction(QString::fromUtf8(spec.label), this);
        action->setObjectName(QString::fromUtf8(spec.objectName)); action->setAutoRepeat(false);
        QList<QKeySequence> bindings{QKeySequence(QString::fromUtf8(spec.key))};
        if (*spec.alias) bindings.append(QKeySequence(QString::fromUtf8(spec.alias)));
        action->setShortcuts(bindings); cube_->window()->addAction(action);
        connect(action, &QAction::triggered, this, std::move(triggered));
        return action;
    }
    QAction* shortcutAction(GuestShortcut kind) const
    {
        switch (kind) {
        case GuestShortcut::Spotify: return spotifyAction_;
        case GuestShortcut::Playback: return playbackAction_;
        case GuestShortcut::Search: return requestAction_;
        case GuestShortcut::Previous: return previousAction_;
        case GuestShortcut::Next: return nextAction_;
        case GuestShortcut::Lyrics: return lyricsAction_;
        default: return nullptr;
        }
    }
    double trackProgress() const
    {
        if (!trackClockKnown_) return 0;
        const double elapsed = trackClockPlaying_ ? std::clamp(double(QDateTime::currentMSecsSinceEpoch()) - trackClockStamp_, 0.0, 10000.0) : 0;
        return std::clamp(trackClockPosition_ + elapsed, 0.0, std::max(0.0, trackClockDuration_));
    }
    void updateTrackClock(const QJsonObject& track)
    {
        const QString key = text(track, "uid", text(track, "uri", text(track, "id", text(track, "title"))));
        double held = trackProgress();
        if (key != trackClockKey_) { trackClockKnown_ = false; held = 0; trackClockKey_ = key; }
        const auto position = track.value(QStringLiteral("progressMs"));
        const auto spotify = state_.value(QStringLiteral("spotify")).toObject();
        const auto playback = state_.value(QStringLiteral("playbackState")).toObject();
        const auto stampValue = track.contains(QStringLiteral("observedAt")) ? track.value(QStringLiteral("observedAt")) : spotify.value(QStringLiteral("observedAt"));
        const double stamp = stampValue.toDouble();
        const bool running = track.value(QStringLiteral("playing")).toBool() && !track.value(QStringLiteral("paused")).toBool()
            && !track.value(QStringLiteral("buffering")).toBool() && !playback.value(QStringLiteral("paused")).toBool()
            && !playback.value(QStringLiteral("buffering")).toBool();
        const bool valid = !key.isEmpty() && position.isDouble() && std::isfinite(position.toDouble()) && position.toDouble() >= 0
            && (!track.contains(QStringLiteral("clockValid")) || track.value(QStringLiteral("clockValid")).toBool())
            && (!playback.contains(QStringLiteral("clockValid")) || playback.value(QStringLiteral("clockValid")).toBool())
            && (!spotify.contains(QStringLiteral("connected")) || spotify.value(QStringLiteral("connected")).toBool())
            && (!running || (stampValue.isDouble() && std::isfinite(stamp) && stamp > 0 && stamp <= QDateTime::currentMSecsSinceEpoch()));
        trackClockTrusted_ = valid;
        if (valid) {
            trackClockKnown_ = true; trackClockPosition_ = position.toDouble(); trackClockStamp_ = stamp;
            trackClockDuration_ = std::max(0.0, number(track, "durationMs", 0)); trackClockPlaying_ = running;
        } else { trackClockPosition_ = held; trackClockPlaying_ = false; }
    }
    struct PlaybackView {
        bool enabled = false, known = false, paused = true;
        QString uid, device, reason;
    };
    PlaybackView playbackView() const
    {
        PlaybackView view;
        const auto now = state_.value(QStringLiteral("nowPlaying")).toObject();
        const auto playback = state_.value(QStringLiteral("playbackState")).toObject();
        const auto spotify = state_.value(QStringLiteral("spotify")).toObject();
        const auto paused = playback.value(QStringLiteral("paused"));
        view.known = paused.isBool() && !now.isEmpty(); view.paused = paused.toBool(true);
        view.uid = text(now, "uid"); view.device = text(playback.value(QStringLiteral("device")).toObject(), "id");
        const auto stampValue = spotify.value(QStringLiteral("observedAt"));
        const double stamp = stampValue.toDouble(), age = QDateTime::currentMSecsSinceEpoch() - stamp;
        const QUrl base(requestUrl());
        const bool local = (base.scheme() == QStringLiteral("http") || base.scheme() == QStringLiteral("https"))
            && (base.host() == QStringLiteral("127.0.0.1") || base.host() == QStringLiteral("localhost") || base.host() == QStringLiteral("::1"));
        if (playbackBusy_) view.reason = QStringLiteral("Provjeravam promjenu reprodukcije…");
        else if (!spotify.value(QStringLiteral("connected")).toBool()) view.reason = QStringLiteral("Spotify nije povezan.");
        else if (!stampValue.isDouble() || !std::isfinite(stamp) || age < -1000 || age > 10000 || stamp < playbackRefreshAfter_
                 || spotify.value(QStringLiteral("stale")).toBool()) view.reason = QStringLiteral("Čekam svježe stanje Spotifyja.");
        else if (now.value(QStringLiteral("buffering")).toBool() || playback.value(QStringLiteral("buffering")).toBool()) view.reason = QStringLiteral("Spotify učitava pjesmu.");
        else if (!view.known || !now.value(QStringLiteral("clockValid")).isBool() || !now.value(QStringLiteral("clockValid")).toBool()
                 || (playback.contains(QStringLiteral("clockValid")) && !playback.value(QStringLiteral("clockValid")).toBool())
                 || (now.value(QStringLiteral("paused")).isBool() && now.value(QStringLiteral("paused")).toBool() != view.paused))
            view.reason = QStringLiteral("Položaj i stanje pjesme još nisu potvrđeni.");
        else if (view.uid.isEmpty() || view.device.isEmpty()) view.reason = QStringLiteral("Čekam točnu pjesmu i uređaj.");
        else if (!local || !QFileInfo::exists(QFileInfo(stateFile_).dir().filePath(QStringLiteral("admin-token"))))
            view.reason = QStringLiteral("Kontrola treba privatnu lokalnu DJ vezu.");
        else view.enabled = true;
        return view;
    }
    void updatePlaybackControl()
    {
        if (!playbackButton_ || !cube_) return;
        const double u = std::clamp(std::min(width() / 1280.0, height() / 800.0), .65, 2.0);
        const double infoWidth = std::min(width() * .53, 690 * u);
        playbackButton_->setGeometry(qRound(34 * u + infoWidth - 190 * u), qRound(27 * u), qRound(190 * u), qRound(43 * u));
        playbackStatus_->setGeometry(qRound(34 * u + infoWidth - 240 * u), qRound(70 * u), qRound(240 * u), qRound(19 * u));
        const auto control = playbackView();
        playbackButton_->setEnabled(control.enabled);
        playbackAction_->setEnabled(control.enabled);
        playbackAction_->setText(control.known && control.paused ? QStringLiteral("Nastavi") : QString::fromUtf8(shortcutSpec(GuestShortcut::Playback).label));
        previousAction_->setEnabled(control.enabled); nextAction_->setEnabled(control.enabled);
        playbackButton_->setText(!control.known ? QStringLiteral("Reprodukcija")
            : control.paused ? QStringLiteral("Pauzirano  ▶") : QStringLiteral("Svira  Ⅱ"));
        playbackButton_->setToolTip(control.enabled ? (control.paused ? QStringLiteral("Nastavi ovu pjesmu na istom uređaju.")
            : QStringLiteral("Pauziraj ovu pjesmu na istom uređaju.")) + QStringLiteral("  ") + shortcutKey(GuestShortcut::Playback) : control.reason);
        playbackButton_->setStyleSheet(controlStyle(u));
        playbackStatus_->setFont(uiFont(u, UiMetrics::small));
        playbackStatus_->setStyleSheet(QStringLiteral("color:#9eafc2; background:transparent;"));
        playbackStatus_->setText(playbackBusy_ ? QStringLiteral("Provjeravam…") : control.enabled ? QString() : QStringLiteral("Čekam potvrđeno stanje"));
        playbackStatus_->setToolTip(control.reason);
        playbackStatus_->setVisible(isVisible()); playbackStatus_->raise();
        playbackButton_->setVisible(isVisible()); playbackButton_->raise();
        const double gap = 6 * u;
        const double preferredWidth = guestShortcuts.size() * 76 * u + gap * (guestShortcuts.size() - 1);
        const double availableWidth = width() - 34 * u - (playbackButton_->geometry().right() + 14 * u);
        const double rowWidth = std::min(std::max(requestCardWidth(width(), u), preferredWidth), std::max(1.0, availableWidth));
        const double rowX = width() - 34 * u - rowWidth;
        const double cell = (rowWidth - gap * (guestShortcuts.size() - 1)) / guestShortcuts.size();
        for (std::size_t i = 0; i < guestShortcuts.size(); ++i) {
            auto* button = shortcutButtons_[i].data(); if (!button) continue;
            const auto kind = static_cast<GuestShortcut>(i); auto* action = shortcutAction(kind);
            const QString label = action->text();
            button->setText(shortcutKey(kind) + QChar('\n') + label); button->setEnabled(action->isEnabled());
            button->setCheckable(action->isCheckable()); button->setChecked(action->isChecked());
            button->setToolTip(kind == GuestShortcut::Lyrics
                ? (lyricsMode_ ? QStringLiteral("Zatvori stihove") : QStringLiteral("Prikaži stihove trenutne pjesme")) + QStringLiteral(" · ") + shortcutKey(kind)
                : action->isEnabled() ? action->text() + QStringLiteral(" · ") + shortcutKey(kind) : control.reason);
            button->setGeometry(qRound(rowX + i * (cell + gap)), qRound(27 * u), qRound(cell), qRound(43 * u));
            button->setStyleSheet(controlStyle(u, UiMetrics::small)); button->setVisible(isVisible()); button->raise();
        }
    }
    void refreshPlaybackState()
    {
        QNetworkRequest request(QUrl(requestUrl()).resolved(QUrl(QStringLiteral("/api/state"))));
        request.setTransferTimeout(8000); request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        auto* reply = controlNetwork_.get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            const auto document = QJsonDocument::fromJson(reply->readAll());
            const auto incoming = document.object(), spotify = incoming.value(QStringLiteral("spotify")).toObject();
            const double previousStamp = number(state_.value(QStringLiteral("spotify")).toObject(), "observedAt", 0);
            const bool revisionCurrent = number(incoming, "revision", -1) >= number(state_, "revision", 0);
            const bool observationCurrent = !spotify.value(QStringLiteral("connected")).toBool()
                || (spotify.value(QStringLiteral("observedAt")).isDouble() && number(spotify, "observedAt", -1) >= previousStamp);
            if (reply->error() == QNetworkReply::NoError && document.isObject() && !spotify.isEmpty()
                && revisionCurrent && observationCurrent) applyState(incoming);
            playbackBusy_ = false; reply->deleteLater(); updatePlaybackControl();
        });
    }
    void setPlaybackPaused()
    {
        dispatchPlayback(QString());
    }
    void navigatePlayback(const QString& direction)
    {
        if (direction != QStringLiteral("next") && direction != QStringLiteral("previous")) return;
        dispatchPlayback(direction);
    }
    void dispatchPlayback(const QString& direction)
    {
        const auto control = playbackView(); if (!control.enabled) return;
        QFile file(QFileInfo(stateFile_).dir().filePath(QStringLiteral("admin-token")));
        if (!file.open(QIODevice::ReadOnly)) { if (desk_) desk_->showNotice(QStringLiteral("Privatna DJ kontrola nije dostupna.")); return; }
        const QByteArray token = file.read(4097).trimmed();
        if (token.isEmpty() || token.size() > 4096 || token.contains('\n') || token.contains('\r')) {
            if (desk_) desk_->showNotice(QStringLiteral("Privatna DJ kontrola nije dostupna.")); return;
        }
        QJsonObject data{{QStringLiteral("expectedCurrentUid"), control.uid}, {QStringLiteral("expectedDeviceId"), control.device},
                         {QStringLiteral("source"), QStringLiteral("native")}};
        const bool navigation = !direction.isEmpty();
        if (navigation) data.insert(QStringLiteral("direction"), direction);
        else data.insert(QStringLiteral("paused"), !control.paused);
        QNetworkRequest request(QUrl(requestUrl()).resolved(QUrl(navigation ? QStringLiteral("/api/admin/skip") : QStringLiteral("/api/admin/playback"))));
        request.setRawHeader("Authorization", "Bearer " + token); request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("X-DJ-Operator", "qtra-native");
        request.setTransferTimeout(12000); request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        playbackBusy_ = true; const qint64 started = QDateTime::currentMSecsSinceEpoch(); updatePlaybackControl();
        auto* reply = controlNetwork_.post(request, QJsonDocument(data).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, started, direction, desired = !control.paused, uid = control.uid, device = control.device] {
            const auto document = QJsonDocument::fromJson(reply->readAll()); const auto result = document.object();
            const auto observed = result.value(QStringLiteral("status")).toObject();
            const auto actual = observed.value(QStringLiteral("playbackState")).toObject();
            const auto current = observed.value(QStringLiteral("nowPlaying")).toObject();
            const bool freshDevice = text(actual.value(QStringLiteral("device")).toObject(), "id") == device
                && number(observed, "observedAt", 0) >= started && number(observed, "observedAt", 0) <= QDateTime::currentMSecsSinceEpoch();
            bool confirmed = reply->error() == QNetworkReply::NoError && result.value(QStringLiteral("ok")).toBool() && freshDevice;
            QString notice;
            if (direction.isEmpty()) {
                confirmed = confirmed && text(result, "action") == QStringLiteral("setPaused") && actual.value(QStringLiteral("paused")).isBool()
                    && actual.value(QStringLiteral("paused")).toBool() == desired && text(current, "uid") == uid;
                notice = desired ? QStringLiteral("Spotify potvrđuje pauzu.") : QStringLiteral("Spotify potvrđuje reprodukciju.");
            } else {
                const QString outcome = text(result, "outcome"), toUid = text(current, "uid");
                const auto position = current.value(QStringLiteral("progressMs"));
                const auto before = result.value(QStringLiteral("before")).toObject();
                const auto beforePosition = before.value(QStringLiteral("positionMs"));
                const auto beforeBuffering = before.value(QStringLiteral("buffering")), nowBuffering = current.value(QStringLiteral("buffering"));
                const auto actualBuffering = actual.value(QStringLiteral("buffering"));
                const double dispatchedAt = number(result, "dispatchedAt", -1), observedAt = number(observed, "observedAt", -1);
                const bool moved = (outcome == direction) && !toUid.isEmpty() && toUid != uid;
                const bool restarted = direction == QStringLiteral("previous") && outcome == QStringLiteral("restarted") && toUid == uid
                    && current.value(QStringLiteral("clockValid")).isBool() && current.value(QStringLiteral("clockValid")).toBool()
                    && before.value(QStringLiteral("clockValid")).isBool() && before.value(QStringLiteral("clockValid")).toBool()
                    && text(before, "currentUid") == uid && text(before, "deviceId") == device
                    && !text(before, "uri").isEmpty() && text(before, "uri") == text(current, "uri")
                    && number(before, "observedAt", -1) >= started && number(before, "observedAt", -1) <= dispatchedAt
                    && dispatchedAt >= started && observedAt >= dispatchedAt
                    && beforeBuffering.isBool() && !beforeBuffering.toBool() && nowBuffering.isBool() && !nowBuffering.toBool()
                    && actualBuffering.isBool() && !actualBuffering.toBool()
                    && beforePosition.isDouble() && std::isfinite(beforePosition.toDouble())
                    && position.isDouble() && std::isfinite(position.toDouble()) && position.toDouble() >= 0
                    && beforePosition.toDouble() - position.toDouble() >= 1000
                    && position.toDouble() <= std::max(0.0, observedAt - dispatchedAt) + 1000;
                confirmed = confirmed && text(result, "action") == QStringLiteral("skip") && text(result, "direction") == direction
                    && result.value(QStringLiteral("changed")).isBool() && result.value(QStringLiteral("changed")).toBool()
                    && text(result, "fromUid") == uid && text(result, "toUid") == toUid
                    && text(result.value(QStringLiteral("device")).toObject(), "id") == device && (moved || restarted);
                notice = restarted ? QStringLiteral("Spotify je vratio pjesmu na početak.")
                    : direction == QStringLiteral("next") ? QStringLiteral("Spotify je prešao na sljedeću pjesmu.")
                    : QStringLiteral("Spotify je vratio prethodnu pjesmu.");
            }
            if (confirmed) {
                if (desk_ && number(observed, "observedAt", 0) >= number(state_.value(QStringLiteral("spotify")).toObject(), "observedAt", 0))
                    desk_->showNotice(notice);
            } else {
                const QString error = text(result, "error", reply->error() == QNetworkReply::NoError
                    ? QStringLiteral("Spotify još nije potvrdio rezultat.") : reply->errorString());
                const bool untouched = result.value(QStringLiteral("dispatched")).isBool() && !result.value(QStringLiteral("dispatched")).toBool();
                if (desk_) desk_->showNotice((untouched ? QStringLiteral("Nije promijenjeno: ") : QStringLiteral("Ishod provjeravam: ")) + error);
            }
            playbackRefreshAfter_ = started;
            reply->deleteLater(); refreshPlaybackState(); updatePlaybackControl();
        });
    }
    void loadArtwork(const QJsonObject& track)
    {
        const QString wanted = coverUrl(track);
        if (wanted == artworkUrl_ && (!artwork_.isNull() || !artworkReply_.isNull() || QDateTime::currentMSecsSinceEpoch() < artworkRetryAt_)) return;
        const quint64 version = ++artworkVersion_;
        if (artworkReply_) artworkReply_->abort();
        artworkReply_ = nullptr; artworkUrl_ = wanted; artwork_ = {}; artworkBytes_.clear(); artworkError_.clear();
        if (wanted.isEmpty()) return;
        if (const auto* cached = artworkCache_.object(wanted)) { artwork_ = *cached; return; }
        QNetworkRequest request{QUrl(wanted)}; request.setTransferTimeout(8000);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        auto* reply = artworkNetwork_.get(request); artworkReply_ = reply;
        const auto consume = [this, reply, version] {
            if (version != artworkVersion_) return;
            constexpr qsizetype limit = 2 * 1024 * 1024;
            artworkBytes_ += reply->read(std::max<qsizetype>(0, limit + 1 - artworkBytes_.size()));
            if (artworkBytes_.size() > limit) { artworkError_ = QStringLiteral("Cover exceeds 2 MiB"); reply->abort(); }
        };
        connect(reply, &QNetworkReply::readyRead, this, consume);
        connect(reply, &QNetworkReply::finished, this, [this, reply, version, wanted, consume] {
            if (version != artworkVersion_) { reply->deleteLater(); return; }
            consume(); artworkReply_ = nullptr;
            if (reply->error() == QNetworkReply::NoError && artworkError_.isEmpty()) {
                artwork_ = decodeCover(artworkBytes_, 320, artworkError_);
                if (!artwork_.isNull()) artworkCache_.insert(wanted, new QImage(artwork_));
            } else if (artworkError_.isEmpty()) artworkError_ = reply->errorString();
            artworkBytes_.clear(); artworkRetryAt_ = QDateTime::currentMSecsSinceEpoch() + 30000;
            reply->deleteLater(); update();
        });
    }
    struct KaraokeFrame {
        bool visible = false, available = false, manual = false;
        QJsonObject sheet, line, next;
        QString status, phase, clockState, title, reason;
        double position = 0, progress = 0, countIn = 0;
        int lineIndex = -1, syllableIndex = -1;
    };
    KaraokeFrame karaokeFrame() const
    {
        KaraokeFrame frame;
        frame.visible = lyricsMode_;
        const auto playback = state_.value(QStringLiteral("nowPlaying")).toObject();
        frame.title = text(playback, "title", QStringLiteral("Nema potvrđene pjesme"));
        frame.status = frame.phase = QStringLiteral("unavailable");
        frame.reason = QStringLiteral("Za ovu pjesmu još nema pripremljenih stihova.");
        const auto sheet = state_.value(QStringLiteral("karaoke")).toObject();
        const auto lines = sheet.value(QStringLiteral("lines")).toArray();
        if (!sheet.value(QStringLiteral("enabled")).toBool() || lines.isEmpty()) return frame;
        const QString source = text(sheet, "source");
        const bool spotify = source == QStringLiteral("spotify");
        if (spotify) {
            const QString uid = text(playback, "uid");
            if (uid.isEmpty()) {
                frame.status = frame.phase = QStringLiteral("waiting");
                frame.reason = QStringLiteral("Čekam potvrđenu trenutnu pjesmu."); return frame;
            }
            // An occurrence stays authoritative across relinked Spotify URIs.
            // A preloaded URI-only sheet is not current until the service binds it.
            if (text(sheet, "trackUid").isEmpty() || text(sheet, "trackUid") != uid
                || text(sheet, "clockState") == QStringLiteral("unmatched")) return frame;
            const auto connection = state_.value(QStringLiteral("spotify")).toObject();
            if (text(sheet, "clockState") == QStringLiteral("disconnected")
                || (connection.contains(QStringLiteral("connected")) && !connection.value(QStringLiteral("connected")).toBool())) {
                frame.status = frame.phase = QStringLiteral("waiting");
                frame.reason = QStringLiteral("Spotify nije povezan. Čekam stanje pjesme."); return frame;
            }
            if (text(sheet, "status") == QStringLiteral("waiting")) return frame;
        } else if (source == QStringLiteral("manual")) {
            frame.manual = true; frame.title = text(sheet, "title", QStringLiteral("Ručna proba"));
        } else return frame;
        frame.sheet = sheet; frame.available = true; frame.reason.clear();
        frame.status = text(frame.sheet, "status", QStringLiteral("waiting"));
        const double stamp = QDateTime::currentMSecsSinceEpoch();
        const auto sampleValue = frame.sheet.value(QStringLiteral("sampledAt"));
        const auto positionValue = frame.sheet.value(QStringLiteral("positionMs"));
        const bool finiteClock = sampleValue.isDouble() && std::isfinite(sampleValue.toDouble()) && sampleValue.toDouble() <= stamp
            && positionValue.isDouble() && std::isfinite(positionValue.toDouble());
        const double sampledAt = number(frame.sheet, "sampledAt", stamp);
        frame.clockState = text(frame.sheet, "clockState", frame.status == QStringLiteral("playing") ? QStringLiteral("running") : frame.status);
        const bool valid = !frame.sheet.contains(QStringLiteral("clockValid")) || frame.sheet.value(QStringLiteral("clockValid")).toBool();
        const bool playbackHeld = spotify && ((playback.contains(QStringLiteral("clockValid")) && !playback.value(QStringLiteral("clockValid")).toBool())
            || playback.value(QStringLiteral("paused")).toBool() || playback.value(QStringLiteral("buffering")).toBool()
            || (playback.contains(QStringLiteral("playing")) && !playback.value(QStringLiteral("playing")).toBool()));
        const double deadline = number(frame.sheet, "clockValidUntil", 0);
        const bool canAdvance = frame.status == QStringLiteral("playing") && finiteClock && valid && !playbackHeld
            && (!spotify || (deadline > 0 && frame.clockState == QStringLiteral("running")));
        const double elapsed = canAdvance ? std::max(0.0, (spotify ? std::min(stamp, deadline) : stamp) - sampledAt) : 0;
        if (frame.status == QStringLiteral("playing") && (!canAdvance || (spotify && stamp >= deadline))) {
            frame.status = QStringLiteral("held");
            if (spotify && canAdvance && stamp >= deadline) frame.clockState = QStringLiteral("stale");
            else if (!finiteClock || !valid) frame.clockState = QStringLiteral("invalid");
            else if (playbackHeld) frame.clockState = QStringLiteral("held");
        }
        // The backend already applies the offset. A negative anchor must stay
        // signed until elapsed time has been added, or delayed lyrics jump early.
        const double duration = std::max(1.0, number(frame.sheet, "durationMs", 1));
        const double signedPosition = number(frame.sheet, "positionMs", 0) + elapsed;
        frame.position = std::clamp(signedPosition, 0.0, duration);
        if (frame.status == QStringLiteral("finished")) frame.position = duration;
        const double cueClock = frame.status == QStringLiteral("finished") ? frame.position : std::min(frame.position, signedPosition);
        frame.lineIndex = 0;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            if (frame.position < number(lines[i].toObject(), "startMs", duration)) break;
            frame.lineIndex = static_cast<int>(i);
        }
        frame.line = lines[frame.lineIndex].toObject();
        if (frame.lineIndex + 1 < lines.size()) frame.next = lines[frame.lineIndex + 1].toObject();
        const auto syllables = frame.line.value(QStringLiteral("syllables")).toArray();
        frame.phase = QStringLiteral("gap");
        if (!syllables.isEmpty()) {
            frame.syllableIndex = 0;
            for (qsizetype i = 0; i < syllables.size(); ++i) {
                const auto syllable = syllables[i].toObject();
                const double start = number(syllable, "startMs", 0), end = number(syllable, "endMs", start + 1);
                if (cueClock < start) {
                    frame.countIn = start - cueClock;
                    frame.phase = i == 0 ? QStringLiteral("count-in") : QStringLiteral("gap");
                    break;
                }
                frame.syllableIndex = static_cast<int>(i);
                frame.progress = unit((frame.position - start) / std::max(1.0, end - start));
                if (frame.position < end) { frame.phase = QStringLiteral("syllable"); break; }
            }
        }
        if (frame.position >= number(frame.line, "endMs", duration)) {
            frame.phase = frame.next.isEmpty() ? QStringLiteral("outro") : QStringLiteral("gap");
            if (!frame.next.isEmpty()) frame.countIn = std::max(0.0, number(frame.next, "startMs", duration) - frame.position);
        }
        if (frame.status == QStringLiteral("waiting") || frame.status == QStringLiteral("finished")) frame.phase = frame.status;
        return frame;
    }
    static double layOutLyrics(QTextLayout& layout, double width, double gap)
    {
        layout.clearLayout();
        QTextOption option; option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); option.setAlignment(Qt::AlignHCenter);
        layout.setTextOption(option); layout.beginLayout();
        double height = 0;
        while (true) {
            QTextLine row = layout.createLine();
            if (!row.isValid()) break;
            row.setLineWidth(width); row.setPosition(QPointF(0, height)); height += row.height() + gap;
        }
        layout.endLayout();
        return std::max(0.0, height - gap);
    }
    void drawKaraoke(QPainter& p, double w, double h, double u, QColor orange)
    {
        karaokeGeometry_ = {};
        const auto frame = karaokeFrame();
        if (!frame.visible) return;
        const QRectF card(34 * u, 292 * u, w - 68 * u,
                          std::min(390 * u, graphicsBottom(w, h, u) - 310 * u));
        if (card.height() < 230 * u || card.width() < 240 * u) return;
        const double v = card.height() / 390;
        const QRectF current = card.adjusted(28 * u, 92 * v, -28 * u, -124 * v);
        const QRectF next(card.x() + 28 * u, card.y() + 308 * v, card.width() - 56 * u, 62 * v);
        p.save(); p.setClipRect(card); panel(p, card, QColor("#7de4ed"), .95);
        p.setFont(uiFont(u, UiMetrics::body, true)); p.setPen(QColor("#7de4ed"));
        const QRectF titleRect = card.adjusted(24 * u, 18 * v, -24 * u, -330 * v);
        p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(p.font()).elidedText((frame.manual ? QStringLiteral("PROBA STIHOVA · ") : QStringLiteral("STIHOVI · ")) + frame.title,
                                                    Qt::ElideRight, qRound(titleRect.width() * .68)));
        const auto rectangle = [](const QRectF& r) { return QJsonArray{r.x(), r.y(), r.width(), r.height()}; };
        if (!frame.available) {
            p.setFont(stageFont(32 * u, true)); p.setPen(QColor("#d5e0ee"));
            p.drawText(current, Qt::AlignCenter | Qt::TextWordWrap, frame.reason);
            p.setFont(uiFont(u, UiMetrics::secondary)); p.setPen(QColor("#aebcd0"));
            p.drawText(next, Qt::AlignCenter | Qt::TextWordWrap,
                       QStringLiteral("Kad stihovi budu spremni, pojavit će se ovdje. %1 zatvara prikaz.").arg(shortcutKey(GuestShortcut::Lyrics)));
            p.restore();
            karaokeGeometry_ = {{QStringLiteral("bounds"), rectangle(card)}, {QStringLiteral("lineBounds"), rectangle(current)}};
            return;
        }
        QString stateLabel;
        if (frame.status == QStringLiteral("waiting")) stateLabel = QStringLiteral("Čekamo pjesmu");
        else if (frame.status == QStringLiteral("paused")) stateLabel = QStringLiteral("Pauza");
        else if (frame.status == QStringLiteral("held")) stateLabel = QStringLiteral("Čekamo položaj pjesme");
        else if (frame.phase == QStringLiteral("finished")) stateLabel = QStringLiteral("Bravo, ekipa!");
        else if (frame.countIn > 0) stateLabel = QStringLiteral("Za %1 s").arg(static_cast<int>(std::ceil(frame.countIn / 1000)));
        else if (frame.phase == QStringLiteral("outro")) stateLabel = QStringLiteral("Završni taktovi");
        p.setFont(uiFont(u, UiMetrics::secondary)); p.setPen(QColor("#b6c5dc"));
        p.drawText(titleRect, Qt::AlignRight | Qt::AlignVCenter, stateLabel);

        const QString words = text(frame.line, "text");
        QFont font = stageFont(58 * u, true);
        QTextLayout layout(words, font);
        double height = layOutLyrics(layout, current.width(), 30 * v);
        while (height > current.height() && font.pixelSize() > std::max(16, qRound(16 * u))) {
            font.setPixelSize(font.pixelSize() - 1); layout.setFont(font);
            height = layOutLyrics(layout, current.width(), 30 * v);
        }
        const QPointF origin(current.left(), current.top() + std::max(0.0, (current.height() - height) / 2));
        QList<QTextLayout::FormatRange> formats;
        const auto syllables = frame.line.value(QStringLiteral("syllables")).toArray();
        int offset = 0, noteOffset = 0, noteLength = 0;
        for (qsizetype i = 0; i < syllables.size(); ++i) {
            const auto syllable = syllables[i].toObject(); const int length = text(syllable, "text").size();
            QTextLayout::FormatRange range; range.start = offset; range.length = length;
            const bool passed = frame.position >= number(syllable, "endMs", 0);
            range.format.setForeground(passed ? QColor("#f7f2ea") : QColor("#97a8c1"));
            if (i == frame.syllableIndex) {
                noteOffset = offset; noteLength = length;
                if (frame.phase == QStringLiteral("syllable")) range.format.setForeground(orange);
            }
            formats.append(range); offset += length;
        }
        layout.setFormats(formats); layOutLyrics(layout, current.width(), 30 * v);
        p.setPen(QColor("#d5e0ee")); layout.draw(&p, origin);
        QPointF note;
        if (!words.isEmpty() && noteLength > 0) {
            const double cursor = std::clamp(noteOffset + noteLength * frame.progress, 0.0, double(words.size()));
            const int left = std::min(static_cast<int>(std::floor(cursor)), static_cast<int>(words.size()) - 1);
            const QTextLine row = layout.lineForTextPosition(left);
            if (row.isValid()) {
                const int right = std::min(left + 1, row.textStart() + row.textLength());
                const double x0 = row.cursorToX(left), x1 = row.cursorToX(right);
                const double bounce = frame.phase == QStringLiteral("syllable") ? std::sin(frame.progress * pi) * 16 * v : 0;
                note = origin + QPointF(x0 + (x1 - x0) * (cursor - left), row.y() - 7 * v - bounce);
                p.setFont(stageFont(32 * u, true)); p.setPen(orange);
                p.drawText(QRectF(note.x() - 25 * u, note.y() - 40 * v, 50 * u, 40 * v), Qt::AlignCenter, QStringLiteral("♪"));
            }
        }
        line(p, QPointF(card.left() + 28 * u, card.top() + 282 * v), QPointF(card.right() - 28 * u, card.top() + 282 * v), QColor("#34475f"), u);
        if (!frame.next.isEmpty()) {
            p.setFont(uiFont(u, UiMetrics::small, true)); p.setPen(QColor("#a8bad2"));
            p.drawText(QRectF(next.x(), card.top() + 286 * v, next.width(), 19 * v), Qt::AlignCenter, QStringLiteral("SLJEDEĆE"));
            QFont nextFont = stageFont(30 * u);
            QTextLayout nextLayout(text(frame.next, "text"), nextFont);
            double nextHeight = layOutLyrics(nextLayout, next.width(), 4 * v);
            while (nextHeight > next.height() && nextFont.pixelSize() > std::max(14, qRound(13 * u))) {
                nextFont.setPixelSize(nextFont.pixelSize() - 1); nextLayout.setFont(nextFont);
                nextHeight = layOutLyrics(nextLayout, next.width(), 4 * v);
            }
            p.setPen(QColor("#aebcd0")); nextLayout.draw(&p, QPointF(next.x(), next.y() + std::max(0.0, (next.height() - nextHeight) / 2)));
        }
        p.restore();
        karaokeGeometry_ = {{QStringLiteral("bounds"), rectangle(card)}, {QStringLiteral("lineBounds"), rectangle(current)},
                            {QStringLiteral("note"), QJsonArray{note.x(), note.y()}}};
    }
    double seconds() const { return clock_.elapsed() / 1000.0; }
    QString palette() const { return text(state_.value(QStringLiteral("scene")).toObject(), "palette"); }
    QString sceneMode() const
    {
        QString mode = text(state_.value(QStringLiteral("scene")).toObject(), "mode", QStringLiteral("auto"));
        if (mode != QStringLiteral("auto")) return mode;
        const std::array<QString, 4> modes{QStringLiteral("orbit"), QStringLiteral("robots"), QStringLiteral("laser"), QStringLiteral("orbit")};
        return modes[static_cast<std::size_t>(seconds() / 85) % modes.size()];
    }
    void applyEffect()
    {
        if (!cube_) return;
        effectIndex_ = std::clamp(state_.value(QStringLiteral("scene")).toObject().value(QStringLiteral("effect")).toInt(0), 0, 7);
        const std::array<QString, 3> actions{QStringLiteral("actionOrbitSphere"), QStringLiteral("actionMobiusRing"), QStringLiteral("actionWireframe")};
        QStringList missing;
        for (int bit = 0; bit < 3; ++bit) {
            auto* action = cube_->window()->findChild<QAction*>(actions[bit]);
            const bool wanted = (effectIndex_ & (1 << bit)) != 0;
            if (!action) { if (wanted) missing.append(actions[bit]); continue; }
            if (!effectBefore_.contains(actions[bit])) effectBefore_.insert(actions[bit], action->isChecked());
            if (action->isChecked() != wanted) action->trigger();
        }
        effectError_ = missing.isEmpty() ? QString() : QStringLiteral("Load scene modules for: ") + missing.join(QStringLiteral(", "));
    }
    void openRequests()
    {
        showRequests(true);
        if (desk_) desk_->focusSearch();
    }
    void showSpotify()
    {
        if (!spotifyAction_->isEnabled() || toggleProcess_) return;
        returnFocus_ = QApplication::focusWidget(); windowToggleError_.clear();
        auto* process = new QProcess(this); toggleProcess_ = process;
        const auto finish = [this, process](bool failed) {
            if (toggleProcess_ != process) return;
            if (failed) {
                windowToggleError_ = QString::fromUtf8(process->readAllStandardError()).trimmed();
                if (windowToggleError_.isEmpty()) windowToggleError_ = process->errorString();
                if (desk_) desk_->showNotice(QStringLiteral("Prebacivanje na Spotify nije uspjelo. Pokušaj ponovno."));
            }
            toggleProcess_ = nullptr; process->deleteLater();
        };
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [finish](int code, QProcess::ExitStatus status) { finish(code != 0 || status != QProcess::NormalExit); });
        connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) finish(true);
        });
        process->start(text(windowToggle_, "program", QStringLiteral("python3")),
                       {text(windowToggle_, "script"), QStringLiteral("spotify"), QStringLiteral("--socket"), text(windowToggle_, "socket"),
                        QStringLiteral("--data"), text(windowToggle_, "data")});
    }
    void loadImage(const QString& file, QString& previous, QImage& target, QSize maxSize)
    {
        if (file == previous && (!target.isNull() || file.isEmpty())) return;
        previous = file; target = {};
        if (file.isEmpty()) return;
        QImageReader reader(file); reader.setAutoTransform(true);
        QSize size = reader.size();
        if (size.isValid() && (size.width() > maxSize.width() || size.height() > maxSize.height()))
            reader.setScaledSize(size.scaled(maxSize, Qt::KeepAspectRatio));
        target = reader.read();
    }
    void tick()
    {
        const double t = seconds();
        if (t >= nextEffectCheck_) { applyEffect(); nextEffectCheck_ = t + 1; }
        if (!cue_.text.isEmpty() && t - cueStart_ >= cue_.seconds) {
            cue_ = {}; setProperty("captionId", QString()); setProperty("captionText", QString());
        }
        if (cue_.text.isEmpty() && !pending_.isEmpty()) {
            cue_ = pending_.dequeue(); cueStart_ = t;
            setProperty("captionId", cue_.id); setProperty("captionText", cue_.text);
        }
        if (cue_.text.isEmpty() && pending_.isEmpty() && t > nextAside_
            && t - lastExternalCue_ > 42 && state_.value(QStringLiteral("autoJokes")).toBool(true)) {
            static const std::array<const char*, 15> asides{
                "Did you mean LYRICS? Večeras da.",
                "Tri desetljeća robotike. I još uvijek ručno biramo pjesme.",
                "DJ ima neuronsku mrežu. Publika ima pravo veta.",
                "Ovaj drop prošao je recenziju. Recenzent 2 traži još basa.",
                "Navines je došao po paletu. Dobio je playlistu.",
                "MORUS nosi teret. Večeras je to refren.",
                "Ako plešeš dovoljno nasumično, to je istraživanje.",
                "Pozicioniranje bez GPS-a: slijedi glasnu osobu.",
                "EagleRed drži visinu. Netko neka drži ritam.",
                "Senzori tvrde da je sve pod kontrolom. Senzori su na godišnjem.",
                "Ovo je jedini eksperiment u kojem je drift poželjan.",
                "Ne, MOJE lijevo.",
                "Biti ili ne biti... u redu za pjesmu. NAO zna taj monolog.",
                "NAO je već pjevao Azru. Ovo mu je samo povratni koncert.",
                "Hexapod-FP traži alku. DJ mu je dao još jedan krug."
            };
            const QString words = QString::fromUtf8(asides[aside_++ % asides.size()]);
            cue_ = {QStringLiteral("aside-%1").arg(aside_), words, std::max(6.0, words.size() / 17.0 + 2.0), 0};
            cueStart_ = t; nextAside_ = t + 48 + (aside_ % 4) * 11;
            setProperty("captionId", cue_.id); setProperty("captionText", cue_.text);
        }
        update();
    }
    double graphicsScale() const
    {
        return std::clamp(number(state_.value(QStringLiteral("scene")).toObject(), "graphicsScale", 1), .75, 2.0);
    }
    QRectF captionRect(double w, double h, double u) const
    {
        if (cue_.text.isEmpty()) return {};
        const QFontMetricsF fm(stageFont(24 * u, true));
        const double widest = std::min(w * .79, 1000 * u);
        const QRectF bounds = fm.boundingRect(QRectF(0, 0, widest - 54 * u, h * .32), Qt::TextWordWrap | Qt::AlignCenter, cue_.text);
        const double width = std::min(widest, std::max(360 * u, bounds.width() + 54 * u));
        const double height = std::min(128 * u, bounds.height() + 36 * u);
        return QRectF((w - width) / 2, h - 134 * u - height, width, height);
    }
    double graphicsBottom(double w, double h, double u) const
    {
        Q_UNUSED(w);
        // Robot paths retain a fixed caption clearance. Photographs extend
        // underneath the captions and have their own bottom-anchored bounds.
        return h - (134 + 128 + 14) * u;
    }
    void drawCaption(QPainter& p, double w, double h, double u, QColor white, QColor orange)
    {
        if (cue_.text.isEmpty()) return;
        const double age = seconds() - cueStart_;
        const double opacity = std::min(smooth(age / .6), smooth((cue_.seconds - age) / .65));
        p.save(); p.setOpacity(opacity);
        const QRectF card = captionRect(w, h, u);
        const QRectF words = card.adjusted(27 * u, 18 * u, -27 * u, -18 * u);
        QFont font = stageFont(24 * u, true);
        while (font.pixelSize() > 12 && QFontMetricsF(font).boundingRect(words, Qt::AlignCenter | Qt::TextWordWrap, cue_.text).height() > words.height())
            font.setPixelSize(font.pixelSize() - 1);
        p.setFont(font);
        panel(p, card, orange, .93);
        p.setPen(white);
        p.drawText(words, Qt::AlignCenter | Qt::TextWordWrap, cue_.text);
        p.restore();
    }
    void drawDelivery(QPainter& p, double w, double h, double u, double t,
                      double pulse, QColor orange, double age)
    {
        const bool air = missionKind_ == 0 || missionKind_ == 3;
        const double approach = smooth(age / 5.5);
        const double leaving = smooth((age - 11.0) / 5.5);
        const double top = 292 * u, bottom = graphicsBottom(w, h, u);
        const double robotScale = std::min((missionKind_ == 3 ? 3.3 : 1.45) * u * graphicsScale(),
                                          std::max(.65, (bottom - top - 36) / (air ? 112 : 90)));
        const double offscreen = 78 * robotScale;
        const double x = age < 11 ? (-offscreen + approach * (w * .5 + offscreen))
                                 : w * .5 + leaving * (w * .5 + offscreen);
        const double minY = top + 32 * robotScale;
        const double maxY = std::max(minY, bottom - (air ? 80 : 42) * robotScale - 36);
        const double y = std::clamp(h * (air ? .40 : .69) - (air ? std::sin(approach * pi) * 32 * u : 0), minY, maxY);
        robot(p, missionKind_, QPointF(x, y), robotScale, t, x, 1, pulse, age < 7.5);
        if (age >= 7.5 && age < 13) {
            const double fade = std::min(1.0, (13 - age) / 2);
            const double delivered = smooth((age - 7.5) / 1.3);
            const QPointF pickup(x + (missionKind_ == 2 ? 47 * robotScale : 0),
                                 y + (air ? 54 : missionKind_ == 2 ? -12 : -39) * robotScale);
            const QPointF deck(w * .5, h * .57);
            const QPointF position = pickup * (1 - delivered) + deck * delivered;
            p.save(); p.setOpacity(fade);
            disc(p, position, 28 * u * graphicsScale(), orange, t * 36); p.restore();
        }
        if (age > 4.5 && age < 11) {
            const QRectF bubble(w * .5 - 205 * u, std::max(top, y - 42 * robotScale - 58 * u), 410 * u, 49 * u);
            panel(p, bubble, orange, .94);
            const QPointF anchor(std::clamp(x, bubble.left() + 12, bubble.right() - 12), bubble.bottom());
            line(p, anchor, QPointF(x, y - 28 * robotScale), alpha(orange, .55), 1.2);
            p.setFont(uiFont(u, UiMetrics::secondary, true)); p.setPen(QColor("#ffe4cc"));
            const QString words = age < 7.5 ? missionText_ : QStringLiteral("Na tri!  Jedan, dva... refren.");
            p.drawText(bubble.adjusted(14 * u, 5 * u, -14 * u, -5 * u),
                       Qt::AlignCenter, QFontMetrics(p.font()).elidedText(words, Qt::ElideRight, qRound(bubble.width() - 28 * u)));
        }
    }
    QRectF drawBook(QPainter& p, double w, double h, double u, QColor orange)
    {
        const auto book = state_.value(QStringLiteral("bookMoment")).toObject();
        const qint64 expires = static_cast<qint64>(number(book, "expiresAt", 0));
        if (book_.isNull() || (expires > 0 && QDateTime::currentMSecsSinceEpoch() >= expires)) return {};
        const QRectF card(0, 0, w, h);
        const QSizeF imageSize = QSizeF(book_.size()).scaled(QSizeF(w * .75, h), Qt::KeepAspectRatio);
        const QRectF destination((w - imageSize.width()) / 2, h - imageSize.height(), imageSize.width(), imageSize.height());
        p.save();
        Q_UNUSED(orange);
        p.setRenderHint(QPainter::SmoothPixmapTransform); p.drawImage(destination, book_);
        QLinearGradient labelShade(0, h - 150 * u, 0, h);
        labelShade.setColorAt(0, QColor(6, 10, 22, 0)); labelShade.setColorAt(1, QColor(6, 10, 22, 245));
        p.fillRect(QRectF(0, h - 150 * u, w, 150 * u), labelShade);
        p.setPen(QColor("#f9e6d5")); p.setFont(uiFont(u, UiMetrics::secondary, true));
        p.drawText(QRectF(16 * u, h - 126 * u, w - 32 * u, 25 * u),
                   Qt::AlignCenter, QFontMetrics(p.font()).elidedText(text(book, "title"), Qt::ElideRight, qRound(w - 32 * u)));
        p.setFont(uiFont(u, UiMetrics::small)); p.setPen(QColor("#a6b6ca"));
        const QString page = book.value(QStringLiteral("page")).toVariant().toString();
        p.drawText(QRectF(0, h - 98 * u, w, 21 * u), Qt::AlignCenter,
                   QStringLiteral("30 godina LARICS-a") + (page.isEmpty() ? QString() : QStringLiteral(" · str. ") + page));
        p.restore();
        return card;
    }
    QPointer<CubeWidget> cube_;
    QPointer<RequestDesk> desk_;
    QPointer<QWidget> returnFocus_;
    QPointer<QProcess> toggleProcess_;
    QPointer<QPushButton> playbackButton_;
    QPointer<QLabel> playbackStatus_;
    std::array<QPointer<QPushButton>, static_cast<std::size_t>(GuestShortcut::Count)> shortcutButtons_;
    QNetworkAccessManager artworkNetwork_{this}, controlNetwork_{this};
    QPointer<QNetworkReply> artworkReply_;
    QCache<QString, QImage> artworkCache_{8};
    QImage artwork_;
    QByteArray artworkBytes_;
    QString artworkUrl_, artworkError_;
    quint64 artworkVersion_ = 0;
    qint64 artworkRetryAt_ = 0, playbackRefreshAfter_ = 0;
    bool playbackBusy_ = false, lyricsMode_ = false;
    QJsonArray queuePreview_;
    QString trackClockKey_;
    double trackClockPosition_ = 0, trackClockStamp_ = 0, trackClockDuration_ = 0;
    bool trackClockKnown_ = false, trackClockPlaying_ = false, trackClockTrusted_ = false;
    QCursor originalCursor_, originalCubeCursor_, originalNativeCursor_;
    QString originalTitle_, stateFile_, fileError_, qrPath_, bookPath_, lastTrack_, lastRequest_, missionText_, effectError_;
    QHash<QString, bool> effectBefore_;
    QJsonObject state_, karaokeGeometry_, windowToggle_;
    QString windowToggleError_;
    QByteArray bytes_;
    QImage qr_, book_;
    QElapsedTimer clock_;
    QTimer frame_, poll_;
    QAction *requestAction_ = nullptr, *fullAction_ = nullptr, *escapeAction_ = nullptr, *spotifyAction_ = nullptr, *returnAction_ = nullptr, *playbackAction_ = nullptr;
    QAction *previousAction_ = nullptr, *nextAction_ = nullptr, *lyricsAction_ = nullptr;
    QRectF requestRect_, qrBounds_, playingBounds_;
    QSet<QString> seenCues_;
    QQueue<Cue> pending_;
    Cue cue_;
    double cueStart_ = 0, lastExternalCue_ = -100, nextAside_ = 18, missionStart_ = -100;
    int missionKind_ = -1;
    int effectIndex_ = 0;
    double nextEffectCheck_ = 0;
    std::size_t aside_ = 0;
    quint64 paints_ = 0;
    bool menuWasVisible_ = true, statusWasVisible_ = true, cursorWasSet_ = false, cubeCursorWasSet_ = false, cursorRefreshQueued_ = false;
};

QPointer<PartyStage> stage;
void release(const RuntimeAgentHost* host)
{
    if (!host || host->abi_version != RUNTIME_AGENT_ABI) return;
    delete stage.data();
    host->complete_json(host->invocation_context, "{\"removed\":true}");
}
void run(const RuntimeAgentHost* host)
{
    if (!host || host->abi_version != RUNTIME_AGENT_ABI) return;
    const QJsonObject request = QJsonDocument::fromJson(QByteArray(host->request_json(host->invocation_context))).object();
    if (request.value(QStringLiteral("restore")).toBool()) { release(host); return; }
    const bool composerRpc = request.value(QStringLiteral("getComposerDraft")).toBool() || request.contains(QStringLiteral("setSearchIntent"));
    if (composerRpc && (!stage || stage->composerDraft().isEmpty())) {
        host->fail(host->invocation_context, "Party stage editor is inactive; configure the stage before composer control.");
        return;
    }
    auto* cube = static_cast<CubeWidget*>(host->find_qobject(host->agent_context, "cubeView"));
    if (!cube) { host->fail(host->invocation_context, "cubeView was not found"); return; }
    if (!stage) stage = new PartyStage(cube,
        text(request, "stateFile", QDir::homePath() + QStringLiteral("/.local/state/larics-dj/state.json")),
        text(request, "requestBaseUrl"));
    else if (request.contains(QStringLiteral("stateFile"))) stage->setStateFile(text(request, "stateFile"));
    if (request.contains(QStringLiteral("state"))) stage->applyState(request.value(QStringLiteral("state")).toObject());
    if (request.contains(QStringLiteral("requestBaseUrl"))) stage->setRequestBaseUrl(text(request, "requestBaseUrl"));
    if (request.contains(QStringLiteral("windowToggle"))) stage->configureWindowToggle(request.value(QStringLiteral("windowToggle")).toObject());
    if (request.contains(QStringLiteral("composerDraft"))) stage->restoreComposerDraft(request.value(QStringLiteral("composerDraft")).toObject());
    if (request.value(QStringLiteral("lyricsMode")).isBool()) stage->setLyricsMode(request.value(QStringLiteral("lyricsMode")).toBool());
    if (request.contains(QStringLiteral("panel"))) stage->showRequests(request.value(QStringLiteral("panel")).toBool());
    if (request.contains(QStringLiteral("fullscreen"))) {
        request.value(QStringLiteral("fullscreen")).toBool() ? cube->window()->showFullScreen() : cube->window()->showNormal();
    }
    if (request.contains(QStringLiteral("capture"))) {
        const QString filename = text(request, "capture");
        if (!cube->window()->grab().save(filename)) { host->fail(host->invocation_context, "Could not save the party window capture"); return; }
    }
    QJsonObject intentResult;
    if (request.contains(QStringLiteral("setSearchIntent"))) intentResult = stage->setSearchIntent(request.value(QStringLiteral("setSearchIntent")).toObject());
    QJsonObject result = stage->status();
    if (!intentResult.isEmpty()) result.insert(QStringLiteral("searchIntentResult"), intentResult);
    if (request.value(QStringLiteral("getComposerDraft")).toBool()) result.insert(QStringLiteral("composerDraft"), stage->composerDraft());
    host->complete_json(host->invocation_context, QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
}
const RuntimeAgentSnippet descriptor{RUNTIME_AGENT_ABI, sizeof(RuntimeAgentSnippet), "LARICS / LYRICS party stage", &run, &release};
} // namespace
extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippet* runtime_agent_snippet_init() { return &descriptor; }
