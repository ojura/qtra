#include "agent/agent_abi.h"
#include "cube_widget.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>

namespace {

QMetaObject::Connection observerConnection;
bool observerInstalled = false;

void run(const RuntimeAgentHostV1* host)
{
    if (host == nullptr || host->abi_version != RUNTIME_AGENT_ABI_V1) {
        return;
    }

    auto* cube = static_cast<CubeWidget*>(
        host->find_qobject(host->agent_context, "cubeView"));
    if (cube == nullptr) {
        host->fail(host->invocation_context, "cubeView was not found");
        return;
    }

    if (!observerInstalled) {
        const auto emitEvent = host->emit_event_json;
        void* stableAgentContext = host->agent_context;
        observerConnection = QObject::connect(
            cube,
            &CubeWidget::frameRendered,
            cube,
            [emitEvent, stableAgentContext](const qulonglong frame, const float angle) {
                if ((frame % 120U) != 0U) {
                    return;
                }
                const QJsonObject payload{
                    {QStringLiteral("frameIndex"), QString::number(frame)},
                    {QStringLiteral("angleDegrees"), angle},
                    {QStringLiteral("source"), QStringLiteral("JIT-installed native observer")},
                };
                const QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
                emitEvent(stableAgentContext, "snippet.observer.frame", json.constData());
            },
            Qt::DirectConnection);
        observerInstalled = true;
    }

    const QJsonObject result{
        {QStringLiteral("installed"), observerInstalled},
        {QStringLiteral("connectionValid"), static_cast<bool>(observerConnection)},
        {QStringLiteral("moduleLifetime"), QStringLiteral("retained until process exit")},
    };
    const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Compact);
    host->complete_json(host->invocation_context, json.constData());
}

const RuntimeAgentSnippetV1 descriptor{
    RUNTIME_AGENT_ABI_V1,
    sizeof(RuntimeAgentSnippetV1),
    "install persistent CubeWidget frame observer",
    &run,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippetV1*
runtime_agent_snippet_init_v1()
{
    return &descriptor;
}
