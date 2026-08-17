#include "agent/agent_abi.h"
#include "cube_widget.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace {

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

    // This file is built/JIT-compiled with -fno-access-control. These reads and
    // writes therefore use the real optimized application's private layout.
    const float angleBefore = cube->m_angleDegrees;
    const float speedBefore = cube->m_angularVelocity;

    QJsonParseError parseError;
    const QJsonDocument request = QJsonDocument::fromJson(
        QByteArray(host->request_json(host->invocation_context)), &parseError);
    if (parseError.error == QJsonParseError::NoError && request.isObject()) {
        const QJsonObject object = request.object();
        if (object.contains(QStringLiteral("setSpeed"))) {
            // Deliberately bypasses CubeWidget::setAngularVelocity(), including
            // its clamp and stateChanged signal. This is an intentional footgun.
            cube->m_angularVelocity = static_cast<float>(
                object.value(QStringLiteral("setSpeed")).toDouble());
        }
        if (object.contains(QStringLiteral("nudgeAngle"))) {
            cube->m_angleDegrees += static_cast<float>(
                object.value(QStringLiteral("nudgeAngle")).toDouble());
        }
    }
    cube->update();

    const QJsonObject result{
        {QStringLiteral("cubeAddress"),
         QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(cube), 0, 16)},
        {QStringLiteral("angleBefore"), angleBefore},
        {QStringLiteral("angleAfter"), cube->m_angleDegrees},
        {QStringLiteral("speedBefore"), speedBefore},
        {QStringLiteral("speedAfter"), cube->m_angularVelocity},
        {QStringLiteral("timerIntervalMs"), cube->m_timer.interval()},
        {QStringLiteral("activePatch"), cube->m_activePatch},
        {QStringLiteral("note"), QStringLiteral("private fields accessed directly")},
    };
    const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Compact);
    host->complete_json(host->invocation_context, json.constData());
}

const RuntimeAgentSnippetV1 descriptor{
    RUNTIME_AGENT_ABI_V1,
    sizeof(RuntimeAgentSnippetV1),
    "inspect/mutate CubeWidget private state",
    &run,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippetV1*
runtime_agent_snippet_init_v1()
{
    return &descriptor;
}
