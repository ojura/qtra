// A party display that follows tools/party_dj's JSON file while qtra keeps
// rendering and analysing the system audio. The fleet's colours and silhouettes
// follow the LARICS Spomenar robots; their cargo tonight is a record.
#include "agent/agent_abi.h"
#include "cube_widget.h"

#include <QAction>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFontMetricsF>
#include <QHash>
#include <QHideEvent>
#include <QImageReader>
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
#include <QPointer>
#include <QProgressBar>
#include <QQueue>
#include <QPushButton>
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
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

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
    explicit RequestDesk(QMainWindow* window) : QDockWidget(QStringLiteral("DJ · pjesme i razgovor"), window)
    {
        setObjectName(QStringLiteral("partyRequestPanel"));
        setAllowedAreas(Qt::RightDockWidgetArea);
        setFeatures(QDockWidget::NoDockWidgetFeatures);
        setFixedWidth(600);
        setStyleSheet(QStringLiteral(
            "QDockWidget,QLabel,QLineEdit,QPlainTextEdit,QListWidget,QTextBrowser,QPushButton { font:29px 'DejaVu Sans'; }"
            "QDockWidget { color:#f7f2ea; background:#101727; }"
            "QDockWidget::title { background:#202a3c; padding:15px; }"
            "QWidget#partyDeskBody { background:#101727; }"
            "QWidget#partySuggestions { background:#080f1d; border-radius:10px; }"
            "QLabel { color:#c1cce0; } QLabel#partyDeskHeading { color:#ff9a58; font-size:30px; font-weight:600; }"
            "QLineEdit,QPlainTextEdit,QListWidget,QTextBrowser,QLabel#partySuggestionHint,QWidget#partySelectedSong { background:#080f1d; color:#f7f2ea; border:1px solid #344158; border-radius:10px; padding:11px; }"
            "QLineEdit:focus,QPlainTextEdit:focus { border-color:#7de4ed; }"
            "QListWidget::item { padding:10px 4px; border-bottom:1px solid #202d43; }"
            "QListWidget::item:hover { background:#203b4b; color:#e5fcff; }"
            "QListWidget::item:selected { background:#3b3040; color:#fff3e5; }"
            "QPushButton { background:#ff8a42; color:#121725; border:0; border-radius:10px; padding:14px; font-weight:600; }"
            "QPushButton:disabled { background:#364258; color:#9ba9be; }"
            "QPushButton:hover { background:#ffad72; }"));
        auto* body = new QWidget(this); body->setObjectName(QStringLiteral("partyDeskBody"));
        auto* layout = new QVBoxLayout(body); layout->setContentsMargins(21, 19, 21, 19); layout->setSpacing(11);
        auto* heading = new QLabel(QStringLiteral("Daj pjesmu. Reci DJ-u."), body);
        heading->setObjectName(QStringLiteral("partyDeskHeading")); heading->setWordWrap(true); layout->addWidget(heading);
        name_ = new QLineEdit(body); name_->setObjectName(QStringLiteral("partyName"));
        name_->setPlaceholderText(QStringLiteral("Tvoje ime (po želji)")); name_->setMaxLength(48);
        QSettings settings(QStringLiteral("qtra"), QStringLiteral("party"));
        guestId_ = settings.value(QStringLiteral("guestId")).toString();
        if (guestId_.isEmpty()) { guestId_ = uuid(); settings.setValue(QStringLiteral("guestId"), guestId_); }
        name_->setText(settings.value(QStringLiteral("name"), QStringLiteral("Laptop")).toString());
        connect(name_, &QLineEdit::editingFinished, this, [this] {
            QSettings(QStringLiteral("qtra"), QStringLiteral("party")).setValue(QStringLiteral("name"), name_->text().trimmed());
        });
        layout->addWidget(name_);
        presence_ = new QLabel(QStringLiteral("Razgovor s DJ-em"), body); presence_->setWordWrap(true); layout->addWidget(presence_);
        presence_->setFixedHeight(73); presence_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        chat_ = new QTextBrowser(body); chat_->setObjectName(QStringLiteral("partyChat"));
        chat_->setOpenExternalLinks(false); chat_->setMinimumHeight(175); layout->addWidget(chat_, 1);
        suggestionArea_ = new QWidget(body); auto* suggestionArea = suggestionArea_;
        suggestionArea->setObjectName(QStringLiteral("partySuggestions"));
        suggestionArea->setFixedHeight(255); suggestionArea->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        auto* suggestionLayout = new QVBoxLayout(suggestionArea); suggestionLayout->setContentsMargins(0, 0, 0, 0); suggestionLayout->setSpacing(0);
        searchActivity_ = new QProgressBar(suggestionArea); searchActivity_->setObjectName(QStringLiteral("partySearchActivity"));
        searchActivity_->setTextVisible(false); searchActivity_->setFixedHeight(5); searchActivity_->setRange(0, 1); searchActivity_->setValue(0);
        searchActivity_->setStyleSheet(QStringLiteral("QProgressBar { background:#162235; border:0; } QProgressBar::chunk { background:#7de4ed; }"));
        suggestionLayout->addWidget(searchActivity_);
        suggestions_ = new QStackedWidget(suggestionArea); suggestions_->setObjectName(QStringLiteral("partySuggestionPages"));
        suggestions_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding); suggestionLayout->addWidget(suggestions_, 1);
        results_ = new QListWidget(suggestions_); results_->setObjectName(QStringLiteral("partyResults"));
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
        magicHelp_ = new QLabel(body); magicHelp_->setObjectName(QStringLiteral("partyMagicHelp"));
        magicHelp_->setWordWrap(true); magicHelp_->setTextFormat(Qt::PlainText); magicHelp_->setFixedHeight(73);
        magicHelp_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); layout->addWidget(magicHelp_);
        send_ = new QPushButton(QStringLiteral("Pošalji poruku (Enter)"), body); send_->setObjectName(QStringLiteral("partySendButton")); layout->addWidget(send_);
        searchDelay_.setSingleShot(true); searchDelay_.setInterval(330);
        connect(message_, &QPlainTextEdit::textChanged, this, [this] { composerChanged(); });
        connect(&searchDelay_, &QTimer::timeout, this, [this] { search(); });
        connect(results_, &QListWidget::itemSelectionChanged, this, [this] { chooseSong(); });
        connect(cancelSong_, &QPushButton::clicked, this, [this] { cancelSong(); });
        connect(send_, &QPushButton::clicked, this, [this] { sendMagic(); });
        notice_ = new QLabel(body);
        notice_->setWordWrap(true); notice_->setTextFormat(Qt::PlainText); notice_->setFixedHeight(73);
        notice_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); layout->addWidget(notice_);
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
    void setBase(const QString& url)
    {
        const QUrl next(url);
        if (next == base_) return;
        base_ = next; cursor_.clear(); messages_ = {}; messageIds_.clear(); chatFetches_ = 0; chat_->clear();
        if (isVisible()) fetchChat();
    }
    QJsonObject status() const
    {
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
                {QStringLiteral("searchCount"), results_->count()}, {QStringLiteral("panelNotice"), notice_->text()},
                {QStringLiteral("composerMode"), selectedTrack_.isEmpty() ? QStringLiteral("message") : QStringLiteral("dedication")},
                {QStringLiteral("suggestionState"), suggestionState_}, {QStringLiteral("selectedTrack"), text(selectedTrack_, "title")}};
    }
    QJsonObject draft() const
    {
        const auto cursor = message_->textCursor();
        return {{QStringLiteral("text"), message_->toPlainText()}, {QStringLiteral("cursor"), cursor.position()},
                {QStringLiteral("anchor"), cursor.anchor()}, {QStringLiteral("query"), savedQuery_},
                {QStringLiteral("queryCursor"), savedQueryCursor_}, {QStringLiteral("queryAnchor"), savedQueryAnchor_},
                {QStringLiteral("selectedTrack"), selectedTrack_}};
    }
    void restoreDraft(const QJsonObject& draft)
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
protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == message_ && (event->type() == QEvent::Move || event->type() == QEvent::Resize))
            updateSuggestionOverlay();
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
    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
    QString name() const { return name_->text().trimmed().isEmpty() ? QStringLiteral("Gost") : name_->text().trimmed(); }
    void updateSuggestionOverlay()
    {
        // This child floats over the transcript and never participates in its
        // layout. Showing results or a selected song cannot move the composer.
        const QRect composer = message_->geometry();
        const QRect chat = chat_->geometry();
        const int height = std::max(1, std::min(255, chat.height()));
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
        suggestionArea_->setVisible(selected || results);
        if (selected || results) suggestionArea_->raise();
    }
    void setNotice(const QString& words)
    {
        notice_->setToolTip(words);
        notice_->setText(QFontMetrics(stageFont(29)).elidedText(words, Qt::ElideRight, 1025));
    }
    void suggestionHint(const QString& words, const QString& state)
    {
        suggestionHint_->setText(words); suggestionHint_->setToolTip(words);
        suggestions_->setCurrentWidget(suggestionHint_); suggestionState_ = state;
        updateSuggestionOverlay();
    }
    void setComposerText(const QString& words, int position, int anchor)
    {
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
        magicHelp_->setText(song ? QStringLiteral("Posveta uz pjesmu (po želji).\nEnter šalje · Shift+Enter: novi red")
                                : QStringLiteral("Odaberi pjesmu ili piši DJ-u.\nEnter: slanje · Shift+Enter: novi red"));
        send_->setText(song ? QStringLiteral("Pošalji pjesmu (Enter)") : QStringLiteral("Pošalji poruku (Enter)"));
        const bool busy = requestBusy_ || messageBusy_;
        send_->setEnabled(!busy && (song || !message_->toPlainText().trimmed().isEmpty()));
        cancelSong_->setEnabled(!busy);
        if (song) {
            searchActivity_->setRange(0, 1); searchActivity_->setValue(0);
            const QFontMetrics fm(stageFont(29));
            selectedTitle_->setText(fm.elidedText(text(selectedTrack_, "title"), Qt::ElideRight, 500));
            selectedTitle_->setToolTip(text(selectedTrack_, "title"));
            selectedArtist_->setText(fm.elidedText(text(selectedTrack_, "artist"), Qt::ElideRight, 500));
            selectedArtist_->setToolTip(text(selectedTrack_, "artist"));
            suggestions_->setCurrentWidget(selectedPage_); suggestionState_ = QStringLiteral("selected");
        }
        updateSuggestionOverlay();
    }
    void composerChanged()
    {
        ++searchVersion_; updateComposerMode();
        if (!selectedTrack_.isEmpty()) { searchDelay_.stop(); return; }
        if (message_->toPlainText().trimmed().size() < 2) {
            { const QSignalBlocker block(results_); results_->clear(); }
            results_->setEnabled(true); searchActivity_->setRange(0, 1); searchActivity_->setValue(0);
            searchDelay_.stop();
            suggestionHint(QStringLiteral("Ovdje se pojavljuju pjesme.\nPoruku možeš odmah poslati DJ-u."), QStringLiteral("empty"));
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
    void chooseSong()
    {
        if (requestBusy_ || messageBusy_ || !selectedTrack_.isEmpty() || !results_->currentItem() || results_->selectedItems().isEmpty()) return;
        const auto track = QJsonObject::fromVariantMap(results_->currentItem()->data(Qt::UserRole).toMap());
        if (text(track, "uri").isEmpty()) return;
        savedQuery_ = message_->toPlainText();
        savedQueryCursor_ = message_->textCursor().position(); savedQueryAnchor_ = message_->textCursor().anchor();
        selectedTrack_ = track; ++searchVersion_; searchDelay_.stop(); setComposerText(QString(), 0, 0);
        updateComposerMode(); setNotice(QStringLiteral("Pjesma je odabrana. Možeš dodati posvetu.")); message_->setFocus();
    }
    void cancelSong()
    {
        if (requestBusy_ || messageBusy_) return;
        selectedTrack_ = {};
        setComposerText(savedQuery_, savedQueryCursor_, savedQueryAnchor_); savedQuery_.clear();
        updateComposerMode(); composerChanged(); setNotice(QStringLiteral("Pjesma nije poslana. Upit je vraćen.")); message_->setFocus();
    }
    void sendMagic()
    {
        if (requestBusy_ || messageBusy_) return;
        selectedTrack_.isEmpty() ? sendMessage() : requestTrack();
    }
    using Done = std::function<void(QJsonObject, QString)>;
    void call(const QString& path, const QJsonObject* data, Done done)
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
    }
    void search()
    {
        if (!selectedTrack_.isEmpty()) return;
        const QString query = message_->toPlainText().simplified().left(160);
        if (query.size() < 2) return;
        const quint64 version = searchVersion_;
        QUrlQuery args; args.addQueryItem(QStringLiteral("q"), query);
        call(QStringLiteral("/api/search?") + args.toString(QUrl::FullyEncoded), nullptr, [this, version](QJsonObject data, QString error) {
            if (version != searchVersion_ || !selectedTrack_.isEmpty()) return;
            results_->setEnabled(true); searchActivity_->setRange(0, 1); searchActivity_->setValue(0);
            const QSignalBlocker block(results_);
            results_->clear();
            if (!error.isEmpty()) {
                suggestionHint(QStringLiteral("Pretraga trenutačno ne radi.\nPoruku i dalje možeš poslati DJ-u."), QStringLiteral("error"));
                setNotice(error); return;
            }
            const QFontMetrics fm(stageFont(29));
            // The results page can still be hidden while its first reply arrives.
            // Measure its already-laid-out stack so the first titles are not
            // elided to a hidden viewport's initial size.
            const int width = std::max(150, suggestions_->width() - 58);
            for (const auto& value : data.value(QStringLiteral("tracks")).toArray()) {
                const QJsonObject track = value.toObject();
                auto* item = new QListWidgetItem(fm.elidedText(text(track, "title"), Qt::ElideRight, width)
                    + QStringLiteral("\n") + fm.elidedText(text(track, "artist"), Qt::ElideRight, width), results_);
                item->setData(Qt::UserRole, track.toVariantMap());
                item->setToolTip(text(track, "title") + QStringLiteral("\n") + text(track, "artist"));
                item->setSizeHint(QSize(0, 95));
            }
            results_->setCurrentRow(-1);
            if (results_->count()) {
                suggestions_->setCurrentWidget(results_); suggestionState_ = QStringLiteral("results");
                updateSuggestionOverlay();
            }
            else suggestionHint(QStringLiteral("Nema pjesama za taj upit.\nPoruku možeš poslati DJ-u."), QStringLiteral("empty"));
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
            setNotice(title + (status == QStringLiteral("queued") ? QStringLiteral(" · dodano u red.") : QStringLiteral(" · zahtjev je stigao DJ-u.")));
            selectedTrack_ = {}; savedQuery_.clear(); lastRequestSignature_.clear();
            if (message_->toPlainText().trimmed() == words) setComposerText(QString(), 0, 0);
            updateComposerMode(); composerChanged();
        });
    }
    void appendMessages(const QJsonArray& incoming)
    {
        bool changed = false;
        for (const auto& value : incoming) {
            const auto message = value.toObject(); const QString id = message.value(QStringLiteral("id")).toVariant().toString();
            if (id.isEmpty()) continue;
            if (messageIds_.contains(id)) {
                for (qsizetype i = 0; i < messages_.size(); ++i) {
                    const auto previous = messages_[i].toObject();
                    if (previous.value(QStringLiteral("id")).toVariant().toString() == id) {
                        if (previous != message) { messages_[i] = message; changed = true; }
                        break;
                    }
                }
                continue;
            }
            messageIds_.insert(id); messages_.append(message); changed = true;
        }
        if (!changed) return;
        auto* scroll = chat_->verticalScrollBar(); const bool atBottom = scroll->value() >= scroll->maximum() - 8;
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
            const QString receipt = !dj && text(message, "status") == QStringLiteral("read")
                ? QStringLiteral(" <span style='color:#8994a6'>✓</span>") : QString();
            html += QStringLiteral("<p style='margin:8px 0 13px'><b style='color:%1'>%2</b>%3<br><span style='color:#f0f2f7'>%4</span></p>")
                .arg(dj ? QStringLiteral("#ff9a58") : QStringLiteral("#7de4ed"), author.toHtmlEscaped(), receipt,
                     text(message, "text").toHtmlEscaped().replace(QChar('\n'), QStringLiteral("<br>")));
        }
        chat_->setHtml(html); scroll->setValue(atBottom ? scroll->maximum() : oldScroll);
    }
    void fetchChat()
    {
        if (chatBusy_) return;
        chatBusy_ = true;
        QString path = QStringLiteral("/api/chat");
        const bool receipts = !cursor_.isEmpty() && (++chatFetches_ % 2 == 0);
        if (!cursor_.isEmpty() && !receipts) { QUrlQuery query; query.addQueryItem(QStringLiteral("after"), cursor_); path += QChar('?') + query.toString(QUrl::FullyEncoded); }
        call(path, nullptr, [this, receipts](QJsonObject data, QString error) {
            chatBusy_ = false;
            if (!error.isEmpty()) { presence_->setText(QStringLiteral("DJ veza: ") + error); return; }
            const auto incoming = data.value(QStringLiteral("messages")).toArray();
            appendMessages(incoming);
            // Only a fetched history response advances the read cursor. A POST
            // can finish while older replies are still in flight.
            // Receipt refreshes revisit recent IDs without skipping any IDs that
            // still need to arrive through the incremental history cursor.
            if (!receipts && !incoming.isEmpty()) cursor_ = incoming.last().toObject().value(QStringLiteral("id")).toVariant().toString();
            const auto dj = data.value(QStringLiteral("dj")).toObject();
            presence_->setText(QStringLiteral("Razgovor s DJ-em · ") + text(dj, "status", QStringLiteral("uživo")));
        });
    }
    void sendMessage()
    {
        const QString words = message_->toPlainText().trimmed(); if (messageBusy_ || words.isEmpty()) return;
        if (words.size() > 1500) { setNotice(QStringLiteral("Poruka može imati do 1500 znakova.")); return; }
        if (words != lastMessage_) { messageId_ = uuid(); lastMessage_ = words; }
        const QJsonObject data{{QStringLiteral("guestId"), guestId_}, {QStringLiteral("name"), name()},
                              {QStringLiteral("text"), words}, {QStringLiteral("requestId"), messageId_}};
        messageBusy_ = true; updateComposerMode();
        call(QStringLiteral("/api/message"), &data, [this, words](QJsonObject data, QString error) {
            messageBusy_ = false; updateComposerMode();
            if (!error.isEmpty()) { setNotice(error); return; }
            appendMessages(QJsonArray{data.value(QStringLiteral("message"))});
            if (message_->toPlainText().trimmed() == words) message_->clear();
            lastMessage_.clear(); setNotice(QString()); fetchChat();
        });
    }
    QNetworkAccessManager network_{this};
    QTimer searchDelay_, chatPoll_;
    QUrl base_{QStringLiteral("http://127.0.0.1:8787")};
    QLineEdit* name_;
    QPlainTextEdit* message_;
    QListWidget* results_;
    QStackedWidget* suggestions_;
    QProgressBar* searchActivity_;
    QWidget *selectedPage_, *suggestionArea_;
    QPushButton *send_, *cancelSong_;
    QLabel *presence_, *notice_, *suggestionHint_, *selectedTitle_, *selectedArtist_, *magicHelp_;
    QTextBrowser* chat_;
    QString guestId_, cursor_, requestId_, lastRequestSignature_, messageId_, lastMessage_;
    QString savedQuery_, suggestionState_;
    int savedQueryCursor_ = 0, savedQueryAnchor_ = 0;
    QJsonObject selectedTrack_;
    QJsonArray messages_;
    QSet<QString> messageIds_;
    quint64 searchVersion_ = 0;
    quint64 chatFetches_ = 0;
    bool chatBusy_ = false, requestBusy_ = false, messageBusy_ = false;
};

class PartyStage final : public QWidget {
public:
    explicit PartyStage(CubeWidget* cube, const QString& stateFile)
        : QWidget(cube), cube_(cube), originalTitle_(cube->window()->windowTitle())
    {
        setObjectName(QStringLiteral("partyStage"));
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
        connect(&poll_, &QTimer::timeout, this, [this] { readState(); });
        poll_.start();
        auto* window = cube->window();
        if (auto* main = qobject_cast<QMainWindow*>(window)) {
            menuWasVisible_ = main->menuBar()->isVisible(); statusWasVisible_ = main->statusBar()->isVisible();
            main->menuBar()->hide(); main->statusBar()->hide();
        }
        requestAction_ = new QAction(QStringLiteral("Zatraži pjesmu"), this);
        requestAction_->setObjectName(QStringLiteral("partyRequestAction"));
        requestAction_->setShortcut(QKeySequence(QStringLiteral("R")));
        window->addAction(requestAction_);
        connect(requestAction_, &QAction::triggered, this, [this] { openRequests(); });
        fullAction_ = new QAction(QStringLiteral("Party fullscreen"), this);
        fullAction_->setObjectName(QStringLiteral("partyFullscreenAction"));
        fullAction_->setShortcut(QKeySequence(QStringLiteral("F11")));
        window->addAction(fullAction_);
        connect(fullAction_, &QAction::triggered, this, [this] {
            QWidget* w = cube_->window();
            w->isFullScreen() ? w->showNormal() : w->showFullScreen();
        });
        escapeAction_ = new QAction(this);
        escapeAction_->setShortcut(QKeySequence(Qt::Key_Escape));
        window->addAction(escapeAction_);
        // Escape is intentionally consumed: the guest desk stays available.
        connect(escapeAction_, &QAction::triggered, this, [] {});
        window->setWindowTitle(QStringLiteral("LARICS / LYRICS · after hours"));
        setStateFile(stateFile);
        showRequests(true);
        show(); raise();
    }
    ~PartyStage() override
    {
        delete desk_.data();
        if (cube_) {
            for (auto it = effectBefore_.begin(); it != effectBefore_.end(); ++it) {
                if (auto* action = cube_->window()->findChild<QAction*>(it.key())) {
                    if (action->isChecked() != it.value()) action->trigger();
                }
            }
            cube_->removeEventFilter(this);
            cube_->window()->setWindowTitle(originalTitle_);
            if (auto* main = qobject_cast<QMainWindow*>(cube_->window())) {
                main->menuBar()->setVisible(menuWasVisible_); main->statusBar()->setVisible(statusWasVisible_);
            }
            for (QAction* a : {requestAction_, fullAction_, escapeAction_}) cube_->window()->removeAction(a);
        }
    }
    void setStateFile(const QString& file) { stateFile_ = file; bytes_.clear(); readState(); }
    void readState()
    {
        QFile file(stateFile_);
        if (!file.open(QIODevice::ReadOnly)) { fileError_ = file.errorString(); return; }
        const QByteArray data = file.read(1024 * 1024 + 1);
        if (data.size() > 1024 * 1024) { fileError_ = QStringLiteral("State exceeds 1 MiB"); return; }
        if (data == bytes_) { fileError_.clear(); return; }
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
                {QStringLiteral("trackTitle"), text(state_.value(QStringLiteral("nowPlaying")).toObject(), "title")},
                {QStringLiteral("bookLoaded"), !book_.isNull()}, {QStringLiteral("qrLoaded"), !qr_.isNull()}};
        result.insert(QStringLiteral("panelVisible"), false);
        if (desk_) {
            const auto values = desk_->status();
            for (auto it = values.begin(); it != values.end(); ++it) result.insert(it.key(), it.value());
        }
        const auto karaoke = karaokeFrame();
        QJsonObject singing{{QStringLiteral("active"), karaoke.visible}, {QStringLiteral("positionMs"), karaoke.position},
            {QStringLiteral("status"), karaoke.status}, {QStringLiteral("phase"), karaoke.phase},
            {QStringLiteral("countInMs"), karaoke.countIn},
            {QStringLiteral("lineIndex"), karaoke.lineIndex}, {QStringLiteral("syllableIndex"), karaoke.syllableIndex},
            {QStringLiteral("text"), text(karaoke.line, "text")}, {QStringLiteral("nextText"), text(karaoke.next, "text")}};
        for (auto it = karaokeGeometry_.begin(); it != karaokeGeometry_.end(); ++it) singing.insert(it.key(), it.value());
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
    void showRequests(bool visible)
    {
        Q_UNUSED(visible);
        if (!desk_) {
            auto* window = qobject_cast<QMainWindow*>(cube_->window());
            if (!window) { QDesktopServices::openUrl(QUrl(requestUrl())); return; }
            desk_ = new RequestDesk(window); desk_->setBase(requestUrl());
        }
        desk_->show();
    }
    QJsonObject composerDraft() const { return desk_ ? desk_->draft() : QJsonObject(); }
    void restoreComposerDraft(const QJsonObject& draft)
    {
        if (!desk_) showRequests(true);
        if (desk_) desk_->restoreDraft(draft);
    }
protected:
    void showEvent(QShowEvent* e) override { frame_.start(); QWidget::showEvent(e); }
    void hideEvent(QHideEvent* e) override { frame_.stop(); QWidget::hideEvent(e); }
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (o == cube_) {
            if (e->type() == QEvent::Resize) { setGeometry(cube_->rect()); raise(); }
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

            p.setFont(stageFont(18 * u, true)); p.setPen(orange);
            p.drawText(QRectF(margin, 22 * u, w * .55, 28 * u), Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("LARICS / LYRICS"));
            p.setFont(stageFont(12 * u)); p.setPen(QColor("#b7bfd1"));
            p.drawText(QRectF(margin, 49 * u, w * .55, 21 * u), Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("after hours   ·   robots have requests too"));

            const auto now = state_.value(QStringLiteral("nowPlaying")).toObject();
            const QString title = text(now, "title", QStringLiteral("The lab is warming up"));
            const QString artist = text(now, "artist", QStringLiteral("Odaberi pjesmu. Roboti čekaju."));
            const double infoWidth = std::min(w * .53, 690 * u);
            const QRectF playing(margin, 89 * u, infoWidth, 105 * u);
            panel(p, playing, orange);
            p.setPen(orange); p.setFont(stageFont(11 * u, true));
            p.drawText(playing.adjusted(19 * u, 12 * u, -19 * u, 0), Qt::AlignTop,
                       now.value(QStringLiteral("playing")).toBool() ? QStringLiteral("Sada svira") : QStringLiteral("Na repertoaru"));
            p.setPen(white); p.setFont(stageFont(25 * u, true));
            p.drawText(playing.adjusted(19 * u, 33 * u, -19 * u, -30 * u), Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetrics(p.font()).elidedText(title, Qt::ElideRight, qRound(infoWidth - 38 * u)));
            p.setPen(QColor("#b9c7db")); p.setFont(stageFont(14 * u));
            p.drawText(playing.adjusted(19 * u, 71 * u, -19 * u, 0), Qt::AlignTop,
                       QFontMetrics(p.font()).elidedText(artist, Qt::ElideRight, qRound(infoWidth - 38 * u)));
            const double duration = number(now, "durationMs", 0);
            if (duration > 0) {
                const double elapsed = now.value(QStringLiteral("playing")).toBool()
                    ? std::max(0.0, double(QDateTime::currentMSecsSinceEpoch()) - number(now, "observedAt", QDateTime::currentMSecsSinceEpoch())) : 0;
                const double progress = unit((number(now, "progressMs", 0) + elapsed) / duration);
                p.setPen(Qt::NoPen); p.setBrush(alpha(orange, .14)); p.drawRoundedRect(QRectF(playing.x() + 17 * u, playing.bottom() - 5 * u, infoWidth - 34 * u, 3 * u), 1.5, 1.5);
                p.setBrush(orange); p.drawRoundedRect(QRectF(playing.x() + 17 * u, playing.bottom() - 5 * u, (infoWidth - 34 * u) * progress, 3 * u), 1.5, 1.5);
            }

            const double qrSide = 102 * u;
            requestRect_ = QRectF(w - margin - 240 * u, 24 * u, 240 * u, 190 * u);
            panel(p, requestRect_, cyan);
            p.setFont(stageFont(16 * u, true)); p.setPen(white);
            p.drawText(requestRect_.adjusted(16 * u, 12 * u, -8 * u, 0), Qt::AlignTop, QStringLiteral("Daj pjesmu."));
            p.setFont(stageFont(11 * u)); p.setPen(QColor("#b9c7db"));
            p.drawText(QRectF(requestRect_.x() + 16 * u, 60 * u, 94 * u, 80 * u),
                       Qt::AlignLeft | Qt::TextWordWrap, QStringLiteral("Skeniraj, pronađi,\npošalji DJ-u."));
            if (!qr_.isNull()) {
                const QRectF qrRect(requestRect_.right() - qrSide - 14 * u, 58 * u, qrSide, qrSide);
                p.fillRect(qrRect.adjusted(-4 * u, -4 * u, 4 * u, 4 * u), Qt::white);
                p.setRenderHint(QPainter::SmoothPixmapTransform, false);
                p.drawImage(qrRect, qr_);
            } else {
                p.setPen(alpha(cyan, .45)); p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(QRectF(requestRect_.right() - qrSide - 14 * u, 58 * u, qrSide, qrSide), 12, 12);
                p.setFont(stageFont(28 * u, true)); p.setPen(cyan);
                p.drawText(QRectF(requestRect_.right() - qrSide - 14 * u, 58 * u, qrSide, qrSide), Qt::AlignCenter, QStringLiteral("♪"));
            }
            p.setFont(stageFont(11 * u, true)); p.setPen(cyan);
            p.drawText(requestRect_.adjusted(16 * u, 148 * u, -4 * u, -4 * u), Qt::AlignTop, QStringLiteral("Zatraži pjesmu · R"));
            const QString guestUrl = text(state_, "requestUrl");
            if (!guestUrl.isEmpty()) {
                p.setFont(stageFont(10 * u)); p.setPen(QColor("#95a5bf"));
                p.drawText(requestRect_.adjusted(16 * u, 170 * u, -10 * u, 0), Qt::AlignTop,
                           QFontMetrics(p.font()).elidedText(guestUrl, Qt::ElideMiddle, qRound(210 * u)));
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

        p.setFont(stageFont(11 * u)); p.setPen(QColor("#a3b3c9"));
        const auto mood = state_.value(QStringLiteral("mood")).toObject();
        QString moodLabel = text(mood, "label", QStringLiteral("Dobra frekvencija"));
        const auto queue = state_.value(QStringLiteral("queue")).toArray();
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
    struct KaraokeFrame {
        bool visible = false;
        QJsonObject sheet, line, next;
        QString status, phase;
        double position = 0, progress = 0, countIn = 0;
        int lineIndex = -1, syllableIndex = -1;
    };
    KaraokeFrame karaokeFrame() const
    {
        KaraokeFrame frame;
        frame.sheet = state_.value(QStringLiteral("karaoke")).toObject();
        const auto lines = frame.sheet.value(QStringLiteral("lines")).toArray();
        if (!frame.sheet.value(QStringLiteral("enabled")).toBool() || lines.isEmpty()) return frame;
        frame.visible = true; frame.status = text(frame.sheet, "status", QStringLiteral("waiting"));
        const double stamp = QDateTime::currentMSecsSinceEpoch();
        const double elapsed = frame.status == QStringLiteral("playing")
            ? std::max(0.0, stamp - number(frame.sheet, "sampledAt", stamp)) : 0;
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
        const QRectF card(34 * u, 240 * u, w - 68 * u,
                          std::min(390 * u, graphicsBottom(w, h, u) - 258 * u));
        if (card.height() < 230 * u || card.width() < 240 * u) return;
        const double v = card.height() / 390;
        const QRectF current = card.adjusted(28 * u, 92 * v, -28 * u, -124 * v);
        const QRectF next(card.x() + 28 * u, card.y() + 308 * v, card.width() - 56 * u, 62 * v);
        p.save(); p.setClipRect(card); panel(p, card, QColor("#7de4ed"), .95);
        p.setFont(stageFont(16 * u, true)); p.setPen(QColor("#7de4ed"));
        const QRectF titleRect = card.adjusted(24 * u, 18 * v, -24 * u, -330 * v);
        p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(p.font()).elidedText(QStringLiteral("KARAOKE · ") + text(frame.sheet, "title"), Qt::ElideRight, qRound(titleRect.width() * .68)));
        QString stateLabel;
        if (frame.status == QStringLiteral("waiting")) stateLabel = QStringLiteral("Čekamo pjesmu");
        else if (frame.status == QStringLiteral("paused")) stateLabel = QStringLiteral("Pauza");
        else if (frame.phase == QStringLiteral("finished")) stateLabel = QStringLiteral("Bravo, ekipa!");
        else if (frame.countIn > 0) stateLabel = QStringLiteral("Za %1 s").arg(static_cast<int>(std::ceil(frame.countIn / 1000)));
        else if (frame.phase == QStringLiteral("outro")) stateLabel = QStringLiteral("Završni taktovi");
        p.setFont(stageFont(13 * u)); p.setPen(QColor("#b6c5dc"));
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
            p.setFont(stageFont(11 * u, true)); p.setPen(QColor("#8496b0"));
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
        const auto rectangle = [](const QRectF& r) { return QJsonArray{r.x(), r.y(), r.width(), r.height()}; };
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
        if (desk_) if (auto* composer = desk_->findChild<QPlainTextEdit*>(QStringLiteral("partyMessage"))) composer->setFocus();
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
        return QRectF((w - width) / 2, h - 102 * u - height, width, height);
    }
    double graphicsBottom(double w, double h, double u) const
    {
        Q_UNUSED(w);
        // Robot paths retain a fixed caption clearance. Photographs extend
        // underneath the captions and have their own bottom-anchored bounds.
        return h - (102 + 128 + 14) * u;
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
        const double top = 230 * u, bottom = graphicsBottom(w, h, u);
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
            p.setFont(stageFont(14 * u, true)); p.setPen(QColor("#ffe4cc"));
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
        QLinearGradient labelShade(0, h - 108 * u, 0, h);
        labelShade.setColorAt(0, QColor(6, 10, 22, 0)); labelShade.setColorAt(1, QColor(6, 10, 22, 245));
        p.fillRect(QRectF(0, h - 108 * u, w, 108 * u), labelShade);
        p.setPen(QColor("#f9e6d5")); p.setFont(stageFont(14 * u, true));
        p.drawText(QRectF(16 * u, h - 80 * u, w - 32 * u, 23 * u),
                   Qt::AlignCenter, QFontMetrics(p.font()).elidedText(text(book, "title"), Qt::ElideRight, qRound(w - 32 * u)));
        p.setFont(stageFont(10 * u)); p.setPen(QColor("#a6b6ca"));
        const QString page = book.value(QStringLiteral("page")).toVariant().toString();
        p.drawText(QRectF(0, h - 54 * u, w, 17 * u), Qt::AlignCenter,
                   QStringLiteral("30 godina LARICS-a") + (page.isEmpty() ? QString() : QStringLiteral(" · str. ") + page));
        p.restore();
        return card;
    }
    QPointer<CubeWidget> cube_;
    QPointer<RequestDesk> desk_;
    QString originalTitle_, stateFile_, fileError_, qrPath_, bookPath_, lastTrack_, lastRequest_, missionText_, effectError_;
    QHash<QString, bool> effectBefore_;
    QJsonObject state_, karaokeGeometry_;
    QByteArray bytes_;
    QImage qr_, book_;
    QElapsedTimer clock_;
    QTimer frame_, poll_;
    QAction *requestAction_ = nullptr, *fullAction_ = nullptr, *escapeAction_ = nullptr;
    QRectF requestRect_;
    QSet<QString> seenCues_;
    QQueue<Cue> pending_;
    Cue cue_;
    double cueStart_ = 0, lastExternalCue_ = -100, nextAside_ = 18, missionStart_ = -100;
    int missionKind_ = -1;
    int effectIndex_ = 0;
    double nextEffectCheck_ = 0;
    std::size_t aside_ = 0;
    quint64 paints_ = 0;
    bool menuWasVisible_ = true, statusWasVisible_ = true;
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
    auto* cube = static_cast<CubeWidget*>(host->find_qobject(host->agent_context, "cubeView"));
    if (!cube) { host->fail(host->invocation_context, "cubeView was not found"); return; }
    if (!stage) stage = new PartyStage(cube, text(request, "stateFile", QDir::homePath() + QStringLiteral("/.local/state/larics-dj/state.json")));
    else if (request.contains(QStringLiteral("stateFile"))) stage->setStateFile(text(request, "stateFile"));
    if (request.contains(QStringLiteral("state"))) stage->applyState(request.value(QStringLiteral("state")).toObject());
    if (request.contains(QStringLiteral("requestBaseUrl"))) stage->setRequestBaseUrl(text(request, "requestBaseUrl"));
    if (request.contains(QStringLiteral("composerDraft"))) stage->restoreComposerDraft(request.value(QStringLiteral("composerDraft")).toObject());
    if (request.contains(QStringLiteral("panel"))) stage->showRequests(request.value(QStringLiteral("panel")).toBool());
    if (request.contains(QStringLiteral("fullscreen"))) {
        request.value(QStringLiteral("fullscreen")).toBool() ? cube->window()->showFullScreen() : cube->window()->showNormal();
    }
    if (request.contains(QStringLiteral("capture"))) {
        const QString filename = text(request, "capture");
        if (!cube->window()->grab().save(filename)) { host->fail(host->invocation_context, "Could not save the party window capture"); return; }
    }
    QJsonObject result = stage->status();
    if (request.value(QStringLiteral("getComposerDraft")).toBool()) result.insert(QStringLiteral("composerDraft"), stage->composerDraft());
    host->complete_json(host->invocation_context, QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
}
const RuntimeAgentSnippet descriptor{RUNTIME_AGENT_ABI, sizeof(RuntimeAgentSnippet), "LARICS / LYRICS party stage", &run, &release};
} // namespace
extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippet* runtime_agent_snippet_init() { return &descriptor; }
