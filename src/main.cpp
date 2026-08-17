#include "agent/runtime_agent.h"
#include "main_window.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QProcessEnvironment>
#include <QSurfaceFormat>

#include <memory>
#include <unistd.h>

namespace {

QString defaultSocketName()
{
    const QString configured = qEnvironmentVariable("QT_RUNTIME_AGENT_SOCKET");
    if (!configured.isEmpty()) {
        return configured;
    }
    return QDir::temp().filePath(
        QStringLiteral("qt-runtime-cube-%1.sock").arg(static_cast<qulonglong>(::getuid())));
}

} // namespace

int main(int argc, char** argv)
{
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    format.setSamples(4);
    format.setOption(QSurfaceFormat::DebugContext);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qt-runtime-cube"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCoreApplication::setOrganizationName(QStringLiteral("OpenAI reference demos"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Qt 6/OpenGL runtime-control, native snippet, and hotpatch reference app"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption socketOption(
        QStringList{QStringLiteral("s"), QStringLiteral("agent-socket")},
        QStringLiteral("Unix-domain socket path for the in-process runtime agent."),
        QStringLiteral("path"),
        defaultSocketName());
    QCommandLineOption noAgentOption(
        QStringLiteral("no-agent"),
        QStringLiteral("Run the visual demo without the runtime-control endpoint."));
    QCommandLineOption unsafeOption(
        QStringLiteral("unsafe-agent"),
        QStringLiteral("Enable raw self-memory read/write and deliberate crash commands."));
    parser.addOption(socketOption);
    parser.addOption(noAgentOption);
    parser.addOption(unsafeOption);
    parser.process(application);

    if (parser.isSet(unsafeOption)) {
        qputenv("QT_RUNTIME_AGENT_UNSAFE", "1");
    }

    MainWindow window;
    std::unique_ptr<RuntimeAgent> agent;
    if (!parser.isSet(noAgentOption)) {
        agent = std::make_unique<RuntimeAgent>(
            &window, window.cubeWidget(), parser.value(socketOption), &application);
        QString error;
        if (!agent->start(error)) {
            qWarning().noquote() << "Runtime agent disabled:" << error;
            agent.reset();
        } else {
            window.setAgentSocket(agent->socketName());
            qInfo().noquote() << "Runtime agent listening on" << agent->socketName();
        }
    }

    window.show();
    return application.exec();
}
