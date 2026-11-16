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
#include <QPointer>
#include <QQueue>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QShowEvent>
#include <QStatusBar>
#include <QTextBrowser>
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
    p.setFont(stageFont(std::clamp(scale * 10, 12.0, 15.0), true));
    p.setPen(alpha(light, .82));
    const QString name = kind == 0 ? QStringLiteral("EagleRed")
        : kind == 1 ? QStringLiteral("VIV") : kind == 2 ? QStringLiteral("Navines") : QStringLiteral("MORUS");
    p.drawText(QRectF(at.x() - 80, at.y() + scale * (carrying && air ? 76 : 34), 160, 22),
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
        setFeatures(QDockWidget::DockWidgetClosable);
        setMinimumWidth(350); setMaximumWidth(480);
        setStyleSheet(QStringLiteral(
            "QDockWidget { color:#f7f2ea; background:#101727; font:14px 'DejaVu Sans'; }"
            "QDockWidget::title { background:#202a3c; padding:12px; }"
            "QWidget#partyDeskBody { background:#101727; }"
            "QLabel { color:#c1cce0; } QLabel#partyDeskHeading { color:#ff9a58; font-size:24px; font-weight:600; }"
            "QLineEdit,QListWidget,QTextBrowser { background:#080f1d; color:#f7f2ea; border:1px solid #344158; border-radius:8px; padding:9px; }"
            "QLineEdit:focus { border-color:#7de4ed; }"
            "QListWidget::item { padding:8px 3px; border-bottom:1px solid #202d43; }"
            "QListWidget::item:selected { background:#3b3040; color:#fff3e5; }"
            "QPushButton { background:#ff8a42; color:#121725; border:0; border-radius:8px; padding:11px; font-weight:600; }"
            "QPushButton:disabled { background:#364258; color:#9ba9be; }"
            "QPushButton:hover { background:#ffad72; }"));
        auto* body = new QWidget(this); body->setObjectName(QStringLiteral("partyDeskBody"));
        auto* layout = new QVBoxLayout(body); layout->setContentsMargins(17, 15, 17, 15); layout->setSpacing(9);
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
        search_ = new QLineEdit(body); search_->setObjectName(QStringLiteral("partySearch"));
        search_->setPlaceholderText(QStringLiteral("Pjesma ili izvođač…")); search_->setMaxLength(160); layout->addWidget(search_);
        searchDelay_.setSingleShot(true); searchDelay_.setInterval(330);
        connect(search_, &QLineEdit::textChanged, this, [this] { ++searchVersion_; searchDelay_.start(); });
        connect(&searchDelay_, &QTimer::timeout, this, [this] { search(); });
        connect(search_, &QLineEdit::returnPressed, this, [this] { searchDelay_.stop(); search(); });
        results_ = new QListWidget(body); results_->setObjectName(QStringLiteral("partyResults"));
        results_->setMinimumHeight(115); results_->setMaximumHeight(195); layout->addWidget(results_);
        dedication_ = new QLineEdit(body); dedication_->setObjectName(QStringLiteral("partyDedication"));
        dedication_->setPlaceholderText(QStringLiteral("Posveta (po želji)")); dedication_->setMaxLength(240); layout->addWidget(dedication_);
        request_ = new QPushButton(QStringLiteral("Pošalji pjesmu DJ-u"), body);
        request_->setObjectName(QStringLiteral("partyRequestButton")); request_->setEnabled(false); layout->addWidget(request_);
        connect(results_, &QListWidget::itemSelectionChanged, this, [this] { request_->setEnabled(results_->currentItem() && !requestBusy_); });
        connect(results_, &QListWidget::itemDoubleClicked, this, [this] { requestTrack(); });
        connect(request_, &QPushButton::clicked, this, [this] { requestTrack(); });
        presence_ = new QLabel(QStringLiteral("Razgovor s DJ-em"), body); presence_->setWordWrap(true); layout->addWidget(presence_);
        chat_ = new QTextBrowser(body); chat_->setObjectName(QStringLiteral("partyChat"));
        chat_->setOpenExternalLinks(false); chat_->setMinimumHeight(140); layout->addWidget(chat_, 1);
        message_ = new QLineEdit(body); message_->setObjectName(QStringLiteral("partyMessage"));
        message_->setPlaceholderText(QStringLiteral("Što želiš čuti? Kako je ekipi?")); message_->setMaxLength(1500); layout->addWidget(message_);
        send_ = new QPushButton(QStringLiteral("Pošalji poruku"), body); send_->setObjectName(QStringLiteral("partySendButton")); layout->addWidget(send_);
        connect(message_, &QLineEdit::returnPressed, this, [this] { sendMessage(); });
        connect(send_, &QPushButton::clicked, this, [this] { sendMessage(); });
        notice_ = new QLabel(QStringLiteral("Escape zatvara ovaj panel. Glazba ide dalje."), body);
        notice_->setWordWrap(true); layout->addWidget(notice_);
        setWidget(body);
        chatPoll_.setInterval(1500);
        connect(&chatPoll_, &QTimer::timeout, this, [this] { fetchChat(); });
        connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
            if (visible) { chatPoll_.start(); fetchChat(); search_->setFocus(); }
            else chatPoll_.stop();
        });
        window->addDockWidget(Qt::RightDockWidgetArea, this); hide();
    }
    void setBase(const QString& url)
    {
        const QUrl next(url);
        if (next == base_) return;
        base_ = next; cursor_.clear(); messages_ = {}; messageIds_.clear(); chat_->clear();
        if (isVisible()) fetchChat();
    }
    QJsonObject status() const
    {
        return {{QStringLiteral("panelVisible"), isVisible()}, {QStringLiteral("chatCount"), messages_.size()},
                {QStringLiteral("searchCount"), results_->count()}, {QStringLiteral("panelNotice"), notice_->text()}};
    }
private:
    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
    QString name() const { return name_->text().trimmed().isEmpty() ? QStringLiteral("Gost") : name_->text().trimmed(); }
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
        const QString query = search_->text().trimmed();
        if (query.size() < 2) { results_->clear(); return; }
        const quint64 version = searchVersion_;
        QUrlQuery args; args.addQueryItem(QStringLiteral("q"), query);
        notice_->setText(QStringLiteral("Tražim pjesme…"));
        call(QStringLiteral("/api/search?") + args.toString(QUrl::FullyEncoded), nullptr, [this, version](QJsonObject data, QString error) {
            if (version != searchVersion_) return;
            results_->clear();
            if (!error.isEmpty()) { notice_->setText(error); return; }
            for (const auto& value : data.value(QStringLiteral("tracks")).toArray()) {
                const QJsonObject track = value.toObject();
                auto* item = new QListWidgetItem(text(track, "title") + QStringLiteral("\n") + text(track, "artist"), results_);
                item->setData(Qt::UserRole, track.toVariantMap());
                item->setToolTip(text(track, "album"));
            }
            notice_->setText(results_->count() ? QStringLiteral("Odaberi pjesmu i pošalji DJ-u.") : QStringLiteral("Nema rezultata. Probaj naslov ili izvođača."));
            if (results_->count()) results_->setCurrentRow(0);
        });
    }
    void requestTrack()
    {
        if (requestBusy_ || !results_->currentItem()) return;
        const auto track = QJsonObject::fromVariantMap(results_->currentItem()->data(Qt::UserRole).toMap());
        const QString uri = text(track, "uri"); if (uri.isEmpty()) return;
        const QString signature = uri + QChar('\n') + name() + QChar('\n') + dedication_->text();
        if (signature != lastRequestSignature_) { requestId_ = uuid(); lastRequestSignature_ = signature; }
        QJsonObject data{{QStringLiteral("requestId"), requestId_}, {QStringLiteral("uri"), uri},
                         {QStringLiteral("requestedBy"), name()}, {QStringLiteral("dedication"), dedication_->text().trimmed()}};
        requestBusy_ = true; request_->setEnabled(false); notice_->setText(QStringLiteral("Šaljem zahtjev…"));
        call(QStringLiteral("/api/request"), &data, [this, title = text(track, "title")](QJsonObject data, QString error) {
            requestBusy_ = false; request_->setEnabled(results_->currentItem());
            if (!error.isEmpty()) { notice_->setText(error); return; }
            const QString status = text(data, "status", text(data.value(QStringLiteral("request")).toObject(), "status"));
            notice_->setText(title + (status == QStringLiteral("queued") ? QStringLiteral(" · dodano u red.") : QStringLiteral(" · zahtjev je stigao DJ-u.")));
            dedication_->clear(); lastRequestSignature_.clear();
        });
    }
    void appendMessages(const QJsonArray& incoming)
    {
        bool changed = false;
        for (const auto& value : incoming) {
            const auto message = value.toObject(); const QString id = message.value(QStringLiteral("id")).toVariant().toString();
            if (id.isEmpty() || messageIds_.contains(id)) continue;
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
            html += QStringLiteral("<p style='margin:8px 0 13px'><b style='color:%1'>%2</b><br><span style='color:#f0f2f7'>%3</span></p>")
                .arg(dj ? QStringLiteral("#ff9a58") : QStringLiteral("#7de4ed"), author.toHtmlEscaped(), text(message, "text").toHtmlEscaped().replace(QChar('\n'), QStringLiteral("<br>")));
        }
        chat_->setHtml(html); scroll->setValue(atBottom ? scroll->maximum() : oldScroll);
    }
    void fetchChat()
    {
        if (chatBusy_) return;
        chatBusy_ = true;
        QString path = QStringLiteral("/api/chat");
        if (!cursor_.isEmpty()) { QUrlQuery query; query.addQueryItem(QStringLiteral("after"), cursor_); path += QChar('?') + query.toString(QUrl::FullyEncoded); }
        call(path, nullptr, [this](QJsonObject data, QString error) {
            chatBusy_ = false;
            if (!error.isEmpty()) { presence_->setText(QStringLiteral("DJ veza: ") + error); return; }
            const auto incoming = data.value(QStringLiteral("messages")).toArray();
            appendMessages(incoming);
            // Only a fetched history response advances the read cursor. A POST
            // can finish while older replies are still in flight.
            if (!incoming.isEmpty()) cursor_ = incoming.last().toObject().value(QStringLiteral("id")).toVariant().toString();
            const auto dj = data.value(QStringLiteral("dj")).toObject();
            presence_->setText(QStringLiteral("Razgovor s DJ-em · ") + text(dj, "status", QStringLiteral("uživo")));
        });
    }
    void sendMessage()
    {
        const QString words = message_->text().trimmed(); if (messageBusy_ || words.isEmpty()) return;
        if (words != lastMessage_) { messageId_ = uuid(); lastMessage_ = words; }
        const QJsonObject data{{QStringLiteral("guestId"), guestId_}, {QStringLiteral("name"), name()},
                              {QStringLiteral("text"), words}, {QStringLiteral("requestId"), messageId_}};
        messageBusy_ = true; send_->setEnabled(false);
        call(QStringLiteral("/api/message"), &data, [this, words](QJsonObject data, QString error) {
            messageBusy_ = false; send_->setEnabled(true);
            if (!error.isEmpty()) { notice_->setText(error); return; }
            appendMessages(QJsonArray{data.value(QStringLiteral("message"))});
            if (message_->text().trimmed() == words) message_->clear();
            lastMessage_.clear(); notice_->setText(QStringLiteral("Poruka je stigla. Odgovor DJ-a pojavit će se ovdje.")); fetchChat();
        });
    }
    QNetworkAccessManager network_{this};
    QTimer searchDelay_, chatPoll_;
    QUrl base_{QStringLiteral("http://127.0.0.1:8787")};
    QLineEdit *name_, *search_, *dedication_, *message_;
    QListWidget* results_;
    QPushButton *request_, *send_;
    QLabel *presence_, *notice_;
    QTextBrowser* chat_;
    QString guestId_, cursor_, requestId_, lastRequestSignature_, messageId_, lastMessage_;
    QJsonArray messages_;
    QSet<QString> messageIds_;
    quint64 searchVersion_ = 0;
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
        connect(escapeAction_, &QAction::triggered, this, [this] {
            if (desk_ && desk_->isVisible()) desk_->hide();
            else if (cube_->window()->isFullScreen()) cube_->window()->showNormal();
        });
        window->setWindowTitle(QStringLiteral("LARICS / LYRICS · after hours"));
        setStateFile(stateFile);
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
        loadImage(text(book, "imagePath"), bookPath_, book_, QSize(1400, 1000));
        if (desk_) desk_->setBase(requestUrl());
        setProperty("revision", state.value(QStringLiteral("revision")).toVariant());
        setProperty("scene", sceneMode());
        setProperty("trackTitle", text(now, "title"));
        applyEffect();
        update();
    }
    QJsonObject status() const
    {
        QJsonObject result{{QStringLiteral("active"), true}, {QStringLiteral("stateFile"), stateFile_},
                {QStringLiteral("revision"), state_.value(QStringLiteral("revision"))},
                {QStringLiteral("scene"), sceneMode()}, {QStringLiteral("captionId"), cue_.id},
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
        return result;
    }
    QString requestUrl() const
    {
        return text(state_, "requestLocalUrl", QStringLiteral("http://127.0.0.1:8787"));
    }
    void showRequests(bool visible)
    {
        if (!desk_) {
            if (!visible) return;
            auto* window = qobject_cast<QMainWindow*>(cube_->window());
            if (!window) { QDesktopServices::openUrl(QUrl(requestUrl())); return; }
            desk_ = new RequestDesk(window); desk_->setBase(requestUrl());
        }
        desk_->setVisible(visible);
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

        const double margin = 34 * u;
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
        requestRect_ = QRectF(w - margin - 240 * u, 24 * u, 240 * u, 167 * u);
        panel(p, requestRect_, cyan);
        p.setFont(stageFont(16 * u, true)); p.setPen(white);
        p.drawText(requestRect_.adjusted(16 * u, 12 * u, -8 * u, 0), Qt::AlignTop, QStringLiteral("Daj pjesmu."));
        p.setFont(stageFont(11 * u)); p.setPen(QColor("#b9c7db"));
        p.drawText(QRectF(requestRect_.x() + 16 * u, 60 * u, 112 * u, 52 * u),
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
        p.drawText(requestRect_.adjusted(16 * u, 127 * u, -4 * u, -4 * u), Qt::AlignTop, QStringLiteral("Zatraži pjesmu · R"));
        const QString guestUrl = text(state_, "requestUrl");
        if (!guestUrl.isEmpty()) {
            p.setFont(stageFont(10 * u)); p.setPen(QColor("#95a5bf"));
            p.drawText(requestRect_.adjusted(16 * u, 146 * u, -10 * u, 0), Qt::AlignTop,
                       QFontMetrics(p.font()).elidedText(guestUrl, Qt::ElideMiddle, qRound(210 * u)));
        }

        drawBook(p, w, h, u, orange);
        const double mission = t - missionStart_;
        if (mission >= 0 && mission < 17) drawDelivery(p, w, h, u, t, pulse, orange, mission);
        if (mode == QStringLiteral("robots")) {
            for (int i = 0; i < 3; ++i) {
                const double x = w * (.20 + i * .30) + std::sin(t * .5 + i) * 24 * u;
                robot(p, i, QPointF(x, h * (.68 + (i == 0 ? -.07 : .03))),
                      (i == 0 ? 1.05 : 1.15) * u, t + i, t * 18, std::cos(t + i), pulse, false);
            }
        }
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
    void openRequests() { showRequests(!desk_ || !desk_->isVisible()); }
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
    void drawCaption(QPainter& p, double w, double h, double u, QColor white, QColor orange)
    {
        if (cue_.text.isEmpty()) return;
        const double age = seconds() - cueStart_;
        const double opacity = std::min(smooth(age / .6), smooth((cue_.seconds - age) / .65));
        p.save(); p.setOpacity(opacity);
        p.setFont(stageFont(24 * u, true));
        const QFontMetricsF fm(p.font());
        const double widest = std::min(w * .79, 1000 * u);
        QRectF bounds = fm.boundingRect(QRectF(0, 0, widest - 54 * u, h * .32), Qt::TextWordWrap | Qt::AlignCenter, cue_.text);
        const double width = std::min(widest, std::max(360 * u, bounds.width() + 54 * u));
        const double height = bounds.height() + 36 * u;
        const QRectF card((w - width) / 2, h - 77 * u - height, width, height);
        panel(p, card, orange, .93);
        p.setPen(white);
        p.drawText(card.adjusted(27 * u, 18 * u, -27 * u, -18 * u),
                   Qt::AlignCenter | Qt::TextWordWrap, cue_.text);
        p.restore();
    }
    void drawDelivery(QPainter& p, double w, double h, double u, double t,
                      double pulse, QColor orange, double age)
    {
        const bool air = missionKind_ == 0 || missionKind_ == 3;
        const double approach = smooth(age / 5.5);
        const double leaving = smooth((age - 11.0) / 5.5);
        const double x = age < 11 ? (-100 * u + approach * (w * .5 + 100 * u))
                                 : w * .5 + leaving * (w * .5 + 120 * u);
        const double y = h * (air ? .34 : .69) - (air ? std::sin(approach * pi) * 48 * u : 0);
        const double robotScale = (missionKind_ == 3 ? 3.3 : 1.45) * u;
        robot(p, missionKind_, QPointF(x, y), robotScale, t, x, 1, pulse, age < 7.5);
        if (age >= 7.5 && age < 13) {
            const double fade = std::min(1.0, (13 - age) / 2);
            const double delivered = smooth((age - 7.5) / 1.3);
            const QPointF pickup(x + (missionKind_ == 2 ? 47 * robotScale : 0),
                                 y + (air ? 54 : missionKind_ == 2 ? -12 : -39) * robotScale);
            const QPointF deck(w * .5, h * .57);
            const QPointF position = pickup * (1 - delivered) + deck * delivered;
            p.save(); p.setOpacity(fade);
            disc(p, position, 28 * u, orange, t * 36); p.restore();
        }
        if (age > 4.5 && age < 11) {
            const QRectF bubble(w * .5 - 205 * u, h * (air ? .23 : .56), 410 * u, 49 * u);
            panel(p, bubble, orange, .94);
            const QPointF anchor(std::clamp(x, bubble.left() + 12, bubble.right() - 12), bubble.bottom());
            line(p, anchor, QPointF(x, y - 36 * u), alpha(orange, .55), 1.2);
            p.setFont(stageFont(14 * u, true)); p.setPen(QColor("#ffe4cc"));
            const QString words = age < 7.5 ? missionText_ : QStringLiteral("Na tri!  Jedan, dva... refren.");
            p.drawText(bubble.adjusted(14 * u, 5 * u, -14 * u, -5 * u),
                       Qt::AlignCenter, QFontMetrics(p.font()).elidedText(words, Qt::ElideRight, qRound(bubble.width() - 28 * u)));
        }
    }
    void drawBook(QPainter& p, double w, double h, double u, QColor orange)
    {
        const auto book = state_.value(QStringLiteral("bookMoment")).toObject();
        const qint64 expires = static_cast<qint64>(number(book, "expiresAt", 0));
        if (book_.isNull() || (expires > 0 && QDateTime::currentMSecsSinceEpoch() >= expires)) return;
        const QRectF card(w * .28, h * .275, w * .44, h * .405);
        p.save();
        panel(p, card, orange, .97);
        const QRectF area = card.adjusted(13 * u, 13 * u, -13 * u, -52 * u);
        const QSizeF imageSize = QSizeF(book_.size()).scaled(area.size(), Qt::KeepAspectRatio);
        const QRectF destination(area.center().x() - imageSize.width() / 2,
                                 area.center().y() - imageSize.height() / 2, imageSize.width(), imageSize.height());
        p.setRenderHint(QPainter::SmoothPixmapTransform); p.drawImage(destination, book_);
        p.setPen(QColor("#f9e6d5")); p.setFont(stageFont(14 * u, true));
        p.drawText(QRectF(card.x() + 16 * u, card.bottom() - 42 * u, card.width() - 32 * u, 23 * u),
                   Qt::AlignCenter, QFontMetrics(p.font()).elidedText(text(book, "title"), Qt::ElideRight, qRound(card.width() - 32 * u)));
        p.setFont(stageFont(10 * u)); p.setPen(QColor("#a6b6ca"));
        const QString page = book.value(QStringLiteral("page")).toVariant().toString();
        p.drawText(QRectF(card.x(), card.bottom() - 22 * u, card.width(), 17 * u), Qt::AlignCenter,
                   QStringLiteral("30 godina LARICS-a") + (page.isEmpty() ? QString() : QStringLiteral(" · str. ") + page));
        p.restore();
    }
    QPointer<CubeWidget> cube_;
    QPointer<RequestDesk> desk_;
    QString originalTitle_, stateFile_, fileError_, qrPath_, bookPath_, lastTrack_, lastRequest_, missionText_, effectError_;
    QHash<QString, bool> effectBefore_;
    QJsonObject state_;
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
    if (request.contains(QStringLiteral("panel"))) stage->showRequests(request.value(QStringLiteral("panel")).toBool());
    if (request.contains(QStringLiteral("fullscreen"))) {
        request.value(QStringLiteral("fullscreen")).toBool() ? cube->window()->showFullScreen() : cube->window()->showNormal();
    }
    if (request.contains(QStringLiteral("capture"))) {
        const QString filename = text(request, "capture");
        if (!cube->window()->grab().save(filename)) { host->fail(host->invocation_context, "Could not save the party window capture"); return; }
    }
    host->complete_json(host->invocation_context, QJsonDocument(stage->status()).toJson(QJsonDocument::Compact).constData());
}
const RuntimeAgentSnippet descriptor{RUNTIME_AGENT_ABI, sizeof(RuntimeAgentSnippet), "LARICS / LYRICS party stage", &run, &release};
} // namespace
extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippet* runtime_agent_snippet_init() { return &descriptor; }
