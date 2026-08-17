#include "agent/agent_abi.h"
#include "cube_widget.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <QThread>

namespace {

QString glString(QOpenGLFunctions* functions, const GLenum name)
{
    const GLubyte* value = functions->glGetString(name);
    return value != nullptr
        ? QString::fromLatin1(reinterpret_cast<const char*>(value))
        : QString();
}

void run(const RuntimeAgentHostV1* host)
{
    if (host == nullptr || host->abi_version != RUNTIME_AGENT_ABI_V1) {
        return;
    }

    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        host->fail(host->invocation_context,
                   "no OpenGL context is current; use executor=render");
        return;
    }

    QOpenGLFunctions* functions = context->functions();
    if (functions == nullptr || !functions->initializeOpenGLFunctions()) {
        host->fail(host->invocation_context, "OpenGL functions are unavailable");
        return;
    }

    GLint viewport[4]{0, 0, 0, 0};
    GLint currentProgram = 0;
    GLint framebuffer = 0;
    GLint samples = 0;
    functions->glGetIntegerv(GL_VIEWPORT, viewport);
    functions->glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
    functions->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    functions->glGetIntegerv(GL_SAMPLES, &samples);

    auto* cube = static_cast<CubeWidget*>(
        host->find_qobject(host->agent_context, "cubeView"));
    const QSurfaceFormat format = context->format();
    const QJsonObject result{
        {QStringLiteral("contextAddress"),
         QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(context), 0, 16)},
        {QStringLiteral("threadAddress"),
         QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(QThread::currentThread()), 0, 16)},
        {QStringLiteral("vendor"), glString(functions, GL_VENDOR)},
        {QStringLiteral("renderer"), glString(functions, GL_RENDERER)},
        {QStringLiteral("version"), glString(functions, GL_VERSION)},
        {QStringLiteral("shadingLanguageVersion"),
         glString(functions, GL_SHADING_LANGUAGE_VERSION)},
        {QStringLiteral("format"), QJsonObject{
            {QStringLiteral("major"), format.majorVersion()},
            {QStringLiteral("minor"), format.minorVersion()},
            {QStringLiteral("profile"), static_cast<int>(format.profile())},
            {QStringLiteral("debugContext"), format.testOption(QSurfaceFormat::DebugContext)},
        }},
        {QStringLiteral("viewport"), QJsonArray{
            viewport[0], viewport[1], viewport[2], viewport[3]}},
        {QStringLiteral("currentProgram"), currentProgram},
        {QStringLiteral("boundFramebuffer"), framebuffer},
        {QStringLiteral("widgetDefaultFramebuffer"),
         cube != nullptr ? static_cast<qint64>(cube->defaultFramebufferObject()) : -1},
        {QStringLiteral("samples"), samples},
        {QStringLiteral("frameIndex"),
         cube != nullptr ? QString::number(cube->frameIndex()) : QString()},
        {QStringLiteral("monotonicNs"),
         QString::number(host->monotonic_time_ns(host->agent_context))},
    };

    const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Compact);
    host->complete_json(host->invocation_context, json.constData());
}

const RuntimeAgentSnippetV1 descriptor{
    RUNTIME_AGENT_ABI_V1,
    sizeof(RuntimeAgentSnippetV1),
    "inspect the current OpenGL render callback",
    &run,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippetV1*
runtime_agent_snippet_init_v1()
{
    return &descriptor;
}
