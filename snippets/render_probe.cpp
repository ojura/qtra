#include "agent/agent_abi.h"
#include "cube_widget.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QSurfaceFormat>
#include <QThread>

namespace {

using GLFunctions = QOpenGLFunctions_4_5_Core;

QString glString(GLFunctions* functions, const GLenum name)
{
    const GLubyte* value = functions->glGetString(name);
    return value != nullptr
        ? QString::fromLatin1(reinterpret_cast<const char*>(value))
        : QString();
}

void run(const RuntimeAgentHost* host)
{
    if (host == nullptr || host->abi_version != RUNTIME_AGENT_ABI) {
        return;
    }

    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        host->fail(host->invocation_context,
                   "no OpenGL context is current; use executor=render");
        return;
    }

    // The versioned class reports resolution failure, unlike the unversioned
    // QOpenGLFunctions whose initializeOpenGLFunctions() returns void. Requires
    // the current context to be at least 4.5 core.
    auto* functions = QOpenGLVersionFunctionsFactory::get<GLFunctions>(context);
    if (functions == nullptr || !functions->initializeOpenGLFunctions()) {
        host->fail(host->invocation_context,
                   "OpenGL 4.5 core functions are unavailable");
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

const RuntimeAgentSnippet descriptor{
    RUNTIME_AGENT_ABI,
    sizeof(RuntimeAgentSnippet),
    "inspect the current OpenGL render callback",
    &run,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippet*
runtime_agent_snippet_init()
{
    return &descriptor;
}
