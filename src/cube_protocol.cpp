#include "cube_protocol.h"

#include "agent/module_manager.h"
#include "agent/runtime_agent.h"
#include "cube_widget.h"
#include "main_window.h"

#include "demo/cube_step_abi.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalSocket>
#include <QString>

#include <cstdint>
#include <functional>
#include <utility>

namespace {

// What every cube command answers with, so a client that changed something and
// a client that only asked read the same fields.
QJsonObject cubeState(const CubeWidget& cube)
{
    return QJsonObject{
        {QStringLiteral("angleDegrees"), cube.angleDegrees()},
        {QStringLiteral("angularVelocity"), cube.angularVelocity()},
        {QStringLiteral("running"), cube.isRunning()},
        {QStringLiteral("wireframe"), cube.isWireframe()},
        {QStringLiteral("frameIndex"), QString::number(cube.frameIndex())},
        {QStringLiteral("activePatch"), cube.activePatch()},
        {QStringLiteral("glVendor"), cube.glVendor()},
        {QStringLiteral("glRenderer"), cube.glRenderer()},
        {QStringLiteral("glVersion"), cube.glVersion()},
        {QStringLiteral("address"),
         QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(&cube), 0, 16)},
    };
}

} // namespace

bool registerCubeProtocol(RuntimeAgent& agent, MainWindow& window, ModuleManager& modules)
{
    CubeWidget& cube = *window.cubeWidget();
    bool registered = true;

    // One entry per command, and the only place this application's set is
    // written down. Each names the parameters it reads, and the agent refuses a
    // request naming anything else before the handler runs.
    const auto add = [&agent, &registered](const char* name,
                                           QStringList parameters,
                                           RuntimeAgent::CommandHandler handler) {
        registered = agent.registerCommand(QString::fromLatin1(name),
                                           std::move(parameters),
                                           std::move(handler))
            && registered;
    };

    // The ABI callbacks are always present, while this application decides which
    // targets they may patch and under which admission policy.
    registered = agent.registerPatchProvider(
                     [&modules](void* target,
                                void* replacement,
                                const quint64 owner,
                                const std::uint32_t flags,
                                RuntimeAgentPatchBinding& out,
                                QString& error) {
                         runtime_agent::PatchBinding binding;
                         const int result = modules.bindReplacement(
                             target, replacement, owner,
                             (flags & RUNTIME_AGENT_PATCH_ACCEPT_INCOMPLETE) != 0,
                             binding, error);
                         if (result == 0) {
                             out.id = binding.id;
                             out.original = binding.original;
                             out.previous = binding.previous;
                         }
                         return result;
                     },
                     [&modules](const std::uint64_t bindingId,
                                const quint64 owner,
                                QString& error) {
                         return modules.releaseBinding(bindingId, owner, error);
                     })
        && registered;

    add("cube.state", {}, [&agent, &cube](QLocalSocket* socket,
                                          const QJsonValue& requestId,
                                          const QJsonObject&) {
        agent.sendSuccess(socket, requestId, cubeState(cube));
    });

    add("cube.pause", {}, [&agent, &cube](QLocalSocket* socket,
                                          const QJsonValue& requestId,
                                          const QJsonObject&) {
        cube.setRunning(false);
        agent.sendSuccess(socket, requestId, cubeState(cube));
    });

    add("cube.resume", {}, [&agent, &cube](QLocalSocket* socket,
                                           const QJsonValue& requestId,
                                           const QJsonObject&) {
        cube.setRunning(true);
        agent.sendSuccess(socket, requestId, cubeState(cube));
    });

    add("cube.reset", {}, [&agent, &cube](QLocalSocket* socket,
                                          const QJsonValue& requestId,
                                          const QJsonObject&) {
        cube.resetCube();
        agent.sendSuccess(socket, requestId, cubeState(cube));
    });

    add("cube.speed", {"degreesPerSecond"}, [&agent, &cube](QLocalSocket* socket,
                                                            const QJsonValue& requestId,
                                                            const QJsonObject& parameters) {
        if (!parameters.contains(QStringLiteral("degreesPerSecond"))) {
            agent.sendError(socket, requestId, QStringLiteral("missing_parameter"),
                            QStringLiteral("degreesPerSecond is required"));
            return;
        }
        cube.setAngularVelocity(
            static_cast<float>(parameters.value(QStringLiteral("degreesPerSecond")).toDouble()));
        agent.sendSuccess(socket, requestId, cubeState(cube));
    });

    add("cube.wireframe", {"enabled"}, [&agent, &cube](QLocalSocket* socket,
                                                       const QJsonValue& requestId,
                                                       const QJsonObject& parameters) {
        cube.setWireframe(parameters.value(QStringLiteral("enabled")).toBool());
        agent.sendSuccess(socket, requestId, cubeState(cube));
    });

    add("cube.capture", {"path"}, [&agent, &cube](QLocalSocket* socket,
                                                  const QJsonValue& requestId,
                                                  const QJsonObject& parameters) {
        QString path = parameters.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) {
            // Only grows, and the pid is ours, so two captures in one run and
            // two runs at once both land on names of their own.
            static quint64 unnamedCaptures = 0;
            ++unnamedCaptures;
            path = QDir::temp().filePath(QStringLiteral("qt-runtime-cube-%1-%2.png")
                                             .arg(QCoreApplication::applicationPid())
                                             .arg(unnamedCaptures));
        }
        path = QFileInfo(path).absoluteFilePath();
        QString error;
        if (!cube.captureFramebuffer(path, &error)) {
            agent.sendError(socket, requestId, QStringLiteral("capture_failed"), error);
            return;
        }
        QFile file(path);
        QByteArray digest;
        if (file.open(QIODevice::ReadOnly)) {
            digest = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
        }
        const QJsonObject result{
            {QStringLiteral("path"), path},
            {QStringLiteral("bytes"), QFileInfo(path).size()},
            {QStringLiteral("sha256"), QString::fromLatin1(digest)},
        };
        agent.publishEvent(QStringLiteral("cube.capture.finished"), result);
        agent.sendSuccess(socket, requestId, result);
    });

    add("patch.load", {"path"}, [&agent, &modules](QLocalSocket* socket,
                                                   const QJsonValue& requestId,
                                                   const QJsonObject& parameters) {
        QString error;
        ModuleManager::LoadedModule* module = modules.loadEntryPatch(
            parameters.value(QStringLiteral("path")).toString(), error);
        if (module == nullptr) {
            agent.sendError(socket, requestId, QStringLiteral("load_failed"), error);
            return;
        }
        const QJsonObject result{
            {QStringLiteral("moduleId"), QString::number(module->id)},
            {QStringLiteral("name"), module->name},
            {QStringLiteral("path"), module->path},
        };
        agent.publishEvent(QStringLiteral("patch.loaded"), result);
        agent.sendSuccess(socket, requestId, result);
    });

    add("patch.activate", {"moduleId", "acceptIncompleteCoverage"},
        [&agent, &modules](QLocalSocket* socket,
                          const QJsonValue& requestId,
                          const QJsonObject& parameters) {
            quint64 moduleId = 0;
            if (!RuntimeAgent::parseUnsignedInteger(
                    parameters.value(QStringLiteral("moduleId")), moduleId)) {
                agent.sendError(socket, requestId, QStringLiteral("invalid_module_id"),
                                QStringLiteral("moduleId must be an unsigned integer"));
                return;
            }
            const bool acceptIncomplete = parameters
                .value(QStringLiteral("acceptIncompleteCoverage"))
                .toBool(false);
            QString error;
            if (!modules.activateEntryPatch(moduleId, acceptIncomplete, error)) {
                agent.sendError(socket, requestId, QStringLiteral("patch_failed"), error);
                return;
            }
            const QJsonObject result = modules.patchStatus();
            agent.publishEvent(QStringLiteral("patch.activated"), result);
            agent.sendSuccess(socket, requestId, result);
        });

    add("patch.rollback", {}, [&agent, &modules](QLocalSocket* socket,
                                                 const QJsonValue& requestId,
                                                 const QJsonObject&) {
        QString error;
        if (!modules.rollback(error)) {
            agent.sendError(socket, requestId, QStringLiteral("rollback_failed"), error);
            return;
        }
        const QJsonObject result = modules.patchStatus();
        agent.publishEvent(QStringLiteral("patch.rolledBack"), result);
        agent.sendSuccess(socket, requestId, result);
    });

    add("patch.status", {}, [&agent, &modules](QLocalSocket* socket,
                                               const QJsonValue& requestId,
                                               const QJsonObject&) {
        agent.sendSuccess(socket, requestId, modules.patchStatus());
    });

    // A snippet asking for the render executor runs with the widget's context
    // current, at the next paint. Queuing is the whole of it, so there is no
    // way for this to fail to schedule.
    registered = agent.registerExecutor(QStringLiteral("render"),
                                        [&cube](std::function<void()> call) {
                                            cube.enqueueRenderCallback(std::move(call));
                                            return true;
                                        })
        && registered;

    // Read on every hello, so a client that connects mid-run learns where the
    // cube actually is.
    registered = agent.registerHelloField(QStringLiteral("cube"),
                                          [&cube] { return QJsonValue(cubeState(cube)); })
        && registered;
    // What a module replacing the step function has to have been compiled
    // against, readable without loading one.
    registered = agent.registerHelloField(
                     QStringLiteral("cubePatchAbi"),
                     [] {
                         return QJsonValue(QStringLiteral("0x%1").arg(
                             CUBE_STEP_ABI, 8, 16, QLatin1Char('0')));
                     })
        && registered;
    registered = agent.registerHelloField(
                     QStringLiteral("sourceDir"),
                     [] { return QJsonValue(QStringLiteral(DEMO_SOURCE_DIR)); })
        && registered;
    registered = agent.registerHelloField(
                     QStringLiteral("buildDir"),
                     [] { return QJsonValue(QStringLiteral(DEMO_BUILD_DIR)); })
        && registered;
    registered = agent.registerHelloField(
                     QStringLiteral("buildType"),
                     [] { return QJsonValue(QStringLiteral(DEMO_BUILD_TYPE)); })
        && registered;
    registered = agent.registerHelloField(
                     QStringLiteral("patch"),
                     [&modules] { return QJsonValue(modules.patchStatus()); })
        && registered;

    QObject::connect(&window, &MainWindow::demoJobStarted, &agent,
                     [&agent](const qulonglong id, const QString& name) {
                         agent.publishEvent(QStringLiteral("operation.started"), QJsonObject{
                             {QStringLiteral("operationId"), QString::number(id)},
                             {QStringLiteral("kind"), QStringLiteral("demoJob")},
                             {QStringLiteral("name"), name},
                         });
                     });
    QObject::connect(&window, &MainWindow::demoJobProgress, &agent,
                     [&agent](const qulonglong id, const int percent) {
                         agent.publishEvent(QStringLiteral("operation.progress"), QJsonObject{
                             {QStringLiteral("operationId"), QString::number(id)},
                             {QStringLiteral("percent"), percent},
                         });
                     });
    QObject::connect(&window, &MainWindow::demoJobFinished, &agent,
                     [&agent](const qulonglong id, const QString& outcome) {
                         agent.publishEvent(QStringLiteral("operation.finished"), QJsonObject{
                             {QStringLiteral("operationId"), QString::number(id)},
                             {QStringLiteral("kind"), QStringLiteral("demoJob")},
                             {QStringLiteral("outcome"), outcome},
                         });
                     });

    // The widget's signals, published through the agent. Sequence numbers,
    // retained history and who is subscribed are all its business; which
    // signals are worth an event is this application's.
    QObject::connect(&cube, &CubeWidget::stateChanged, &agent, [&agent, &cube] {
        agent.publishEvent(QStringLiteral("cube.stateChanged"), cubeState(cube));
    });
    QObject::connect(&cube, &CubeWidget::frameRendered, &agent,
                     [&agent](const qulonglong frame, const float angle) {
                         // High-rate events are deliberately throttled. A real
                         // client can install a native observer snippet when it
                         // needs every frame.
                         if ((frame % 60U) == 0U) {
                             agent.publishEvent(QStringLiteral("cube.frame"), QJsonObject{
                                 {QStringLiteral("frameIndex"), QString::number(frame)},
                                 {QStringLiteral("angleDegrees"), angle},
                             });
                         }
                     });
    QObject::connect(&cube, &CubeWidget::activePatchChanged, &agent,
                     [&agent, &modules](const QString& name) {
                         agent.publishEvent(QStringLiteral("patch.changed"), QJsonObject{
                             {QStringLiteral("name"), name},
                             {QStringLiteral("status"), modules.patchStatus()},
                         });
                     });
    QObject::connect(&cube, &CubeWidget::glInitialized, &agent, [&agent, &cube] {
        agent.publishEvent(QStringLiteral("render.glInitialized"), QJsonObject{
            {QStringLiteral("vendor"), cube.glVendor()},
            {QStringLiteral("renderer"), cube.glRenderer()},
            {QStringLiteral("version"), cube.glVersion()},
        });
    });
    QObject::connect(&cube, &CubeWidget::glMessage, &agent,
                     [&agent](const QString& message,
                              const int severity,
                              const int source,
                              const int type,
                              const quint32 id) {
                         agent.publishEvent(QStringLiteral("render.glMessage"), QJsonObject{
                             {QStringLiteral("message"), message},
                             {QStringLiteral("severity"), severity},
                             {QStringLiteral("source"), source},
                             {QStringLiteral("type"), type},
                             {QStringLiteral("id"), static_cast<qint64>(id)},
                         });
                     });

    return registered;
}
