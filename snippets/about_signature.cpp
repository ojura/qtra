// Adds a line to the application's About box in a process that is already
// running, without editing or rebuilding the application.
//
// The obvious way to do this would be to replace the About action's handler,
// but the wording it shows lives in a lambda in main_window.cpp and a snippet
// cannot read a string out of compiled code. Replacing the handler therefore
// means restating the application's own text, and the copy goes stale the
// moment the application changes it.
//
// Filtering instead leaves the application to build its dialog exactly as it
// always does, and edits the result on the way to the screen: an application
// event filter sees QEvent::Show for every widget, including the QMessageBox
// that QMessageBox::about() constructs each time, so the line can be appended
// to whatever text that box happens to carry.
//
// {"line": "..."} sets the text, default "claude was here".
// {"restore": true} takes the filter back off.

#include "agent/agent_abi.h"

#include <QApplication>
#include <QEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QObject>
#include <QString>

#include <utility>

namespace {

// No Q_OBJECT: eventFilter is an ordinary virtual and nothing here needs a
// signal, a slot, or a property, so the module needs no moc output.
class AboutFilter final : public QObject {
public:
    explicit AboutFilter(QString line) : m_line(std::move(line)) {}

    [[nodiscard]] const QString& line() const { return m_line; }
    void setLine(QString line) { m_line = std::move(line); }
    [[nodiscard]] int applied() const { return m_applied; }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Show) {
            if (auto* box = qobject_cast<QMessageBox*>(watched)) {
                const QString text = box->text();
                if (!m_line.isEmpty() && !text.contains(m_line)) {
                    box->setText(text + QStringLiteral("\n\n") + m_line);
                    ++m_applied;
                }
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QString m_line;
    int m_applied = 0;
};

AboutFilter* filter = nullptr;

void run(const RuntimeAgentHostV1* host)
{
    if (host == nullptr || host->abi_version != RUNTIME_AGENT_ABI_V1) {
        return;
    }
    auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (application == nullptr) {
        host->fail(host->invocation_context, "no QApplication in this process");
        return;
    }

    const QJsonObject request =
        QJsonDocument::fromJson(QByteArray(host->request_json(host->invocation_context))).object();

    const auto complete = [host](QJsonObject result) {
        host->complete_json(host->invocation_context,
                            QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    };

    if (request.value(QStringLiteral("restore")).toBool()
        || request.value(QStringLiteral("remove")).toBool()) {
        if (filter == nullptr) {
            complete(QJsonObject{
                {QStringLiteral("removed"), false},
                {QStringLiteral("note"), QStringLiteral("no filter was installed")},
            });
            return;
        }
        application->removeEventFilter(filter);
        const int applied = filter->applied();
        delete filter;
        filter = nullptr;
        complete(QJsonObject{
            {QStringLiteral("removed"), true},
            {QStringLiteral("timesApplied"), applied},
            {QStringLiteral("note"), QStringLiteral("the About box shows its original text again")},
        });
        return;
    }

    const QString line = request.contains(QStringLiteral("line"))
        ? request.value(QStringLiteral("line")).toString()
        : QStringLiteral("claude was here");

    if (filter != nullptr) {
        filter->setLine(line);
        complete(QJsonObject{
            {QStringLiteral("installed"), true},
            {QStringLiteral("line"), filter->line()},
            {QStringLiteral("timesApplied"), filter->applied()},
            {QStringLiteral("note"), QStringLiteral("the filter was already installed; line updated")},
        });
        return;
    }

    filter = new AboutFilter(line);
    application->installEventFilter(filter);
    complete(QJsonObject{
        {QStringLiteral("installed"), true},
        {QStringLiteral("line"), filter->line()},
        {QStringLiteral("appliesTo"), QStringLiteral("every QMessageBox shown from now on")},
    });
}

const RuntimeAgentSnippetV1 descriptor{
    RUNTIME_AGENT_ABI_V1,
    sizeof(RuntimeAgentSnippetV1),
    "sign the About box without rebuilding the application",
    &run,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippetV1*
runtime_agent_snippet_init_v1()
{
    return &descriptor;
}
