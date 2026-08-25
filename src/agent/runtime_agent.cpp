#include "agent/runtime_agent.h"

#include "cube_widget.h"
#include "main_window.h"

#include <QAbstractButton>
#include <QAction>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QProcessEnvironment>
#include <QTimer>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <sys/uio.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr qsizetype maximumRequestBytes = 1024 * 1024;
constexpr qsizetype maximumUnsafeTransferBytes = 64 * 1024;
constexpr qsizetype maximumEventHistory = 1024;
constexpr double maximumExactJsonInteger = 9'007'199'254'740'991.0;

QString pointerString(const void* pointer)
{
    return QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(pointer), 0, 16);
}

bool parseAddress(const QJsonValue& value, quintptr& address)
{
    bool ok = false;
    if (value.isString()) {
        QString text = value.toString().trimmed();
        int base = 10;
        if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
            text.remove(0, 2);
            base = 16;
        }
        address = static_cast<quintptr>(text.toULongLong(&ok, base));
        return ok;
    }
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (std::isfinite(number) && number >= 0.0
            && number <= maximumExactJsonInteger && std::floor(number) == number) {
            address = static_cast<quintptr>(number);
            return true;
        }
    }
    return false;
}

bool parseUnsignedInteger(const QJsonValue& value, quint64& result)
{
    if (value.isString()) {
        const QString text = value.toString().trimmed();
        if (text.isEmpty()) {
            return false;
        }
        bool ok = false;
        const quint64 parsed = text.toULongLong(&ok, 10);
        if (ok) {
            result = parsed;
        }
        return ok;
    }
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (std::isfinite(number) && number >= 0.0
            && number <= maximumExactJsonInteger && std::floor(number) == number) {
            result = static_cast<quint64>(number);
            return true;
        }
    }
    return false;
}

QJsonObject parseObjectOrRaw(const QByteArray& json)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error == QJsonParseError::NoError && document.isObject()) {
        return document.object();
    }
    return QJsonObject{{QStringLiteral("raw"), QString::fromUtf8(json)}};
}

QString errnoText(const char* operation)
{
    return QStringLiteral("%1 failed: %2 (errno=%3)")
        .arg(QString::fromLatin1(operation), QString::fromLocal8Bit(std::strerror(errno)))
        .arg(errno);
}

} // namespace

RuntimeAgent::RuntimeAgent(MainWindow* window,
                           CubeWidget* cube,
                           QString socketName,
                           QObject* parent)
    : QObject(parent)
    , m_window(window)
    , m_cube(cube)
    , m_socketName(std::move(socketName))
    , m_registry(window, this)
    , m_unsafeEnabled(qEnvironmentVariableIntValue("QT_RUNTIME_AGENT_UNSAFE") == 1)
    , m_modules(cube)
{
    setObjectName(QStringLiteral("runtimeAgent"));
    m_monotonicClock.start();

    connect(&m_server, &QLocalServer::newConnection,
            this, &RuntimeAgent::acceptConnections);

    connect(m_cube, &CubeWidget::stateChanged, this, [this] {
        publishEvent(QStringLiteral("cube.stateChanged"), cubeState());
    });
    connect(m_cube, &CubeWidget::frameRendered, this,
            [this](const qulonglong frame, const float angle) {
                // High-rate events are deliberately throttled. A real client can
                // install a native observer snippet when it needs every frame.
                if ((frame % 60U) == 0U) {
                    publishEvent(QStringLiteral("cube.frame"), QJsonObject{
                        {QStringLiteral("frameIndex"), QString::number(frame)},
                        {QStringLiteral("angleDegrees"), angle},
                    });
                }
            });
    connect(m_cube, &CubeWidget::activePatchChanged, this, [this](const QString& name) {
        publishEvent(QStringLiteral("patch.changed"), QJsonObject{
            {QStringLiteral("name"), name},
            {QStringLiteral("status"), m_modules.patchStatus()},
        });
    });
    connect(m_cube, &CubeWidget::glInitialized, this, [this] {
        publishEvent(QStringLiteral("render.glInitialized"), QJsonObject{
            {QStringLiteral("vendor"), m_cube->glVendor()},
            {QStringLiteral("renderer"), m_cube->glRenderer()},
            {QStringLiteral("version"), m_cube->glVersion()},
        });
    });
    connect(m_cube, &CubeWidget::glMessage, this,
            [this](const QString& message,
                   const int severity,
                   const int source,
                   const int type,
                   const quint32 id) {
                publishEvent(QStringLiteral("render.glMessage"), QJsonObject{
                    {QStringLiteral("message"), message},
                    {QStringLiteral("severity"), severity},
                    {QStringLiteral("source"), source},
                    {QStringLiteral("type"), type},
                    {QStringLiteral("id"), static_cast<qint64>(id)},
                });
            });

    connect(m_window, &MainWindow::demoJobStarted, this,
            [this](const qulonglong id, const QString& name) {
                publishEvent(QStringLiteral("operation.started"), QJsonObject{
                    {QStringLiteral("operationId"), QString::number(id)},
                    {QStringLiteral("kind"), QStringLiteral("demoJob")},
                    {QStringLiteral("name"), name},
                });
            });
    connect(m_window, &MainWindow::demoJobProgress, this,
            [this](const qulonglong id, const int percent) {
                publishEvent(QStringLiteral("operation.progress"), QJsonObject{
                    {QStringLiteral("operationId"), QString::number(id)},
                    {QStringLiteral("percent"), percent},
                });
            });
    connect(m_window, &MainWindow::demoJobFinished, this,
            [this](const qulonglong id, const QString& outcome) {
                publishEvent(QStringLiteral("operation.finished"), QJsonObject{
                    {QStringLiteral("operationId"), QString::number(id)},
                    {QStringLiteral("kind"), QStringLiteral("demoJob")},
                    {QStringLiteral("outcome"), outcome},
                });
            });

    for (QAction* action : m_window->findChildren<QAction*>()) {
        (void)m_registry.idFor(action);
        connect(action, &QAction::triggered, this, [this, action](const bool checked) {
            publishEvent(QStringLiteral("action.triggered"), QJsonObject{
                {QStringLiteral("objectName"), action->objectName()},
                {QStringLiteral("text"), action->text()},
                {QStringLiteral("checked"), checked},
            });
        });
    }
}

RuntimeAgent::~RuntimeAgent()
{
    // Qt drops a destroyed receiver's connections in ~QObject, which runs after
    // these members are gone. Anything the members emit on the way out would
    // still reach the lambdas installed in the constructor, so cut those first.
    if (m_cube != nullptr) {
        disconnect(m_cube, nullptr, this, nullptr);
    }
    if (m_window != nullptr) {
        disconnect(m_window, nullptr, this, nullptr);
    }

    // The client sockets are children of m_server, so ~QLocalServer deletes
    // them, and that runs after m_clients has already been destroyed. Each
    // socket aborts on the way out and emits disconnected, which would reach
    // removeClient. Cut those connections while every member is still alive.
    for (auto iterator = m_clients.cbegin(); iterator != m_clients.cend(); ++iterator) {
        disconnect(iterator.key(), nullptr, this, nullptr);
    }
    m_clients.clear();

    const bool ownedEndpoint = m_server.isListening();
    m_server.close();
    if (ownedEndpoint) {
        QLocalServer::removeServer(m_socketName);
    }
}

bool RuntimeAgent::start(QString& error)
{
    if (m_socketName.isEmpty()) {
        error = QStringLiteral("socket name is empty");
        return false;
    }

    // Do not unlink another live agent's endpoint. QLocalServer::removeServer()
    // is appropriate only after a failed probe indicates a stale filesystem
    // socket left by a crashed process.
    if (QFileInfo::exists(m_socketName)) {
        QLocalSocket probe;
        probe.connectToServer(m_socketName);
        if (probe.waitForConnected(100)) {
            probe.disconnectFromServer();
            error = QStringLiteral("runtime-agent socket is already in use: %1")
                        .arg(m_socketName);
            return false;
        }
        if (!QLocalServer::removeServer(m_socketName)) {
            error = QStringLiteral("could not remove stale runtime-agent socket: %1")
                        .arg(m_socketName);
            return false;
        }
    }
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server.listen(m_socketName)) {
        error = m_server.errorString();
        return false;
    }
    return true;
}

QObject* RuntimeAgent::findObject(const QString& objectName) const
{
    if (QObject* object = m_registry.byObjectName(objectName); object != nullptr) {
        return object;
    }
    QObject* application = QCoreApplication::instance();
    return application != nullptr && application->objectName() == objectName
        ? application : nullptr;
}

void RuntimeAgent::publishEvent(const QString& name, const QJsonObject& data)
{
    QJsonObject message{
        {QStringLiteral("event"), name},
        {QStringLiteral("sequence"), QString::number(++m_eventSequence)},
        {QStringLiteral("monotonicNs"), QString::number(m_monotonicClock.nsecsElapsed())},
        {QStringLiteral("data"), data},
    };

    m_eventHistory.enqueue(message);
    while (m_eventHistory.size() > maximumEventHistory) {
        m_eventHistory.dequeue();
    }

    for (auto iterator = m_clients.begin(); iterator != m_clients.end(); ++iterator) {
        if (clientWantsEvent(iterator.value(), name)) {
            sendObject(iterator.key(), message);
        }
    }
}

void RuntimeAgent::acceptConnections()
{
    while (m_server.hasPendingConnections()) {
        QLocalSocket* socket = m_server.nextPendingConnection();
        if (socket == nullptr) {
            continue;
        }
        m_clients.insert(socket, ClientState{});
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
            readClient(socket);
        });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            removeClient(socket);
        });
        sendObject(socket, QJsonObject{
            {QStringLiteral("event"), QStringLiteral("agent.connected")},
            {QStringLiteral("sequence"), QString::number(++m_eventSequence)},
            {QStringLiteral("monotonicNs"), QString::number(m_monotonicClock.nsecsElapsed())},
            {QStringLiteral("data"), hello()},
        });
    }
}

void RuntimeAgent::readClient(QLocalSocket* socket)
{
    auto iterator = m_clients.find(socket);
    if (iterator == m_clients.end()) {
        return;
    }

    iterator->input += socket->readAll();
    if (iterator->input.size() > maximumRequestBytes) {
        sendError(socket, QJsonValue(), QStringLiteral("request_too_large"),
                  QStringLiteral("request buffer exceeded 1 MiB"));
        socket->disconnectFromServer();
        return;
    }

    while (true) {
        const qsizetype newline = iterator->input.indexOf('\n');
        if (newline < 0) {
            break;
        }
        QByteArray line = iterator->input.left(newline).trimmed();
        iterator->input.remove(0, newline + 1);
        if (!line.isEmpty()) {
            handleLine(socket, line);
        }
    }
}

void RuntimeAgent::removeClient(QLocalSocket* socket)
{
    m_clients.remove(socket);
    socket->deleteLater();
}

void RuntimeAgent::handleLine(QLocalSocket* socket, const QByteArray& line)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        sendError(socket, QJsonValue(), QStringLiteral("invalid_json"),
                  parseError.errorString());
        return;
    }

    const QJsonObject request = document.object();
    const QJsonValue requestId = request.value(QStringLiteral("id"));
    const QString command = request.value(QStringLiteral("command")).toString();
    const QJsonObject parameters = request.value(QStringLiteral("params")).toObject();
    if (command.isEmpty()) {
        sendError(socket, requestId, QStringLiteral("missing_command"),
                  QStringLiteral("request.command must be a non-empty string"));
        return;
    }
    dispatchRequest(socket, requestId, command, parameters);
}

void RuntimeAgent::dispatchRequest(QLocalSocket* socket,
                                   const QJsonValue& requestId,
                                   const QString& command,
                                   const QJsonObject& parameters)
{
    if (command == QStringLiteral("hello")) {
        sendSuccess(socket, requestId, hello());
        return;
    }
    if (command == QStringLiteral("help")) {
        sendSuccess(socket, requestId, commandList());
        return;
    }
    if (command == QStringLiteral("cube.state")) {
        sendSuccess(socket, requestId, cubeState());
        return;
    }
    if (command == QStringLiteral("cube.pause")) {
        m_cube->setRunning(false);
        sendSuccess(socket, requestId, cubeState());
        return;
    }
    if (command == QStringLiteral("cube.resume")) {
        m_cube->setRunning(true);
        sendSuccess(socket, requestId, cubeState());
        return;
    }
    if (command == QStringLiteral("cube.reset")) {
        m_cube->resetCube();
        sendSuccess(socket, requestId, cubeState());
        return;
    }
    if (command == QStringLiteral("cube.speed")) {
        if (!parameters.contains(QStringLiteral("degreesPerSecond"))) {
            sendError(socket, requestId, QStringLiteral("missing_parameter"),
                      QStringLiteral("degreesPerSecond is required"));
            return;
        }
        m_cube->setAngularVelocity(
            static_cast<float>(parameters.value(QStringLiteral("degreesPerSecond")).toDouble()));
        sendSuccess(socket, requestId, cubeState());
        return;
    }
    if (command == QStringLiteral("cube.wireframe")) {
        m_cube->setWireframe(parameters.value(QStringLiteral("enabled")).toBool());
        sendSuccess(socket, requestId, cubeState());
        return;
    }
    if (command == QStringLiteral("cube.capture")) {
        QString path = parameters.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) {
            path = QDir::temp().filePath(
                QStringLiteral("qt-runtime-cube-%1-%2.png")
                    .arg(QCoreApplication::applicationPid())
                    .arg(m_eventSequence + 1));
        }
        path = QFileInfo(path).absoluteFilePath();
        QString error;
        if (!m_cube->captureFramebuffer(path, &error)) {
            sendError(socket, requestId, QStringLiteral("capture_failed"), error);
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
        publishEvent(QStringLiteral("cube.capture.finished"), result);
        sendSuccess(socket, requestId, result);
        return;
    }

    if (command == QStringLiteral("object.tree")) {
        QString error;
        const bool hasSelector = parameters.contains(QStringLiteral("objectName"))
            || parameters.contains(QStringLiteral("id"));
        QObject* root = hasSelector ? resolveObject(parameters, error)
                                    : static_cast<QObject*>(m_window.data());
        if (root == nullptr) {
            sendError(socket, requestId, QStringLiteral("object_not_found"), error);
            return;
        }
        sendSuccess(socket, requestId,
                    m_registry.tree(root, parameters.value(QStringLiteral("maxDepth")).toInt(8)));
        return;
    }
    if (command == QStringLiteral("object.list")) {
        sendSuccess(socket, requestId, m_registry.flatList());
        return;
    }
    if (command == QStringLiteral("object.describe")) {
        QString error;
        QObject* object = resolveObject(parameters, error);
        if (object == nullptr) {
            sendError(socket, requestId, QStringLiteral("object_not_found"), error);
            return;
        }
        sendSuccess(socket, requestId, m_registry.describe(
            object, parameters.value(QStringLiteral("includeValues")).toBool(true)));
        return;
    }
    if (command == QStringLiteral("object.get")) {
        QString error;
        QObject* object = resolveObject(parameters, error);
        if (object == nullptr) {
            sendError(socket, requestId, QStringLiteral("object_not_found"), error);
            return;
        }
        const QString propertyName = parameters.value(QStringLiteral("property")).toString();
        if (propertyName.isEmpty()) {
            sendError(socket, requestId, QStringLiteral("missing_parameter"),
                      QStringLiteral("property is required"));
            return;
        }
        const QVariant value = object->property(propertyName.toUtf8().constData());
        if (!value.isValid()) {
            sendError(socket, requestId, QStringLiteral("property_not_found"), propertyName);
            return;
        }
        sendSuccess(socket, requestId, ObjectRegistry::variantToJson(value));
        return;
    }
    if (command == QStringLiteral("object.set")) {
        QString error;
        QObject* object = resolveObject(parameters, error);
        if (object == nullptr) {
            sendError(socket, requestId, QStringLiteral("object_not_found"), error);
            return;
        }
        const QString propertyName = parameters.value(QStringLiteral("property")).toString();
        if (propertyName.isEmpty()) {
            sendError(socket, requestId, QStringLiteral("missing_parameter"),
                      QStringLiteral("property is required"));
            return;
        }
        const QByteArray propertyUtf8 = propertyName.toUtf8();
        const int propertyIndex = object->metaObject()->indexOfProperty(propertyUtf8.constData());
        bool written = false;
        if (propertyIndex >= 0) {
            const QMetaProperty property = object->metaObject()->property(propertyIndex);
            QVariant value;
            if (!ObjectRegistry::jsonToPropertyValue(
                    parameters.value(QStringLiteral("value")), property, value, error)) {
                sendError(socket, requestId, QStringLiteral("conversion_failed"), error);
                return;
            }
            written = property.write(object, value);
        } else {
            (void)object->setProperty(
                propertyUtf8.constData(),
                parameters.value(QStringLiteral("value")).toVariant());
            // QObject::setProperty() returns false when it successfully creates
            // a dynamic property, so verify its presence instead of trusting
            // the return value for the undeclared-property path.
            written = object->dynamicPropertyNames().contains(propertyUtf8);
        }
        if (!written) {
            sendError(socket, requestId, QStringLiteral("property_write_failed"), propertyName);
            return;
        }
        sendSuccess(socket, requestId, m_registry.describe(object));
        return;
    }
    if (command == QStringLiteral("object.invoke")) {
        QString error;
        QObject* object = resolveObject(parameters, error);
        if (object == nullptr) {
            sendError(socket, requestId, QStringLiteral("object_not_found"), error);
            return;
        }
        const QByteArray methodName = parameters.value(QStringLiteral("method")).toString().toUtf8();
        bool invoked = false;
        QString matchedSignature;
        const QMetaObject* metaObject = object->metaObject();
        for (int index = 0; index < metaObject->methodCount(); ++index) {
            const QMetaMethod method = metaObject->method(index);
            if (method.name() == methodName && method.parameterCount() == 0) {
                invoked = method.invoke(object, Qt::DirectConnection);
                matchedSignature = QString::fromLatin1(method.methodSignature());
                break;
            }
        }
        if (!invoked) {
            sendError(socket, requestId, QStringLiteral("invoke_failed"),
                      QStringLiteral("no invokable zero-argument method named %1")
                          .arg(QString::fromUtf8(methodName)));
            return;
        }
        sendSuccess(socket, requestId, QJsonObject{
            {QStringLiteral("invoked"), true},
            {QStringLiteral("signature"), matchedSignature},
        });
        return;
    }
    if (command == QStringLiteral("action.trigger")) {
        QString error;
        QObject* object = resolveObject(parameters, error);
        QAction* action = qobject_cast<QAction*>(object);
        if (action == nullptr) {
            sendError(socket, requestId, QStringLiteral("not_an_action"), error.isEmpty()
                ? QStringLiteral("selected object is not a QAction") : error);
            return;
        }
        action->trigger();
        sendSuccess(socket, requestId, QJsonObject{
            {QStringLiteral("objectName"), action->objectName()},
            {QStringLiteral("checked"), action->isChecked()},
        });
        return;
    }
    if (command == QStringLiteral("widget.click")) {
        QString error;
        QObject* object = resolveObject(parameters, error);
        QAbstractButton* button = qobject_cast<QAbstractButton*>(object);
        if (button == nullptr) {
            sendError(socket, requestId, QStringLiteral("not_a_button"), error.isEmpty()
                ? QStringLiteral("selected object is not a QAbstractButton") : error);
            return;
        }
        button->click();
        sendSuccess(socket, requestId, true);
        return;
    }

    if (command == QStringLiteral("event.subscribe")) {
        auto iterator = m_clients.find(socket);
        if (iterator == m_clients.end()) {
            sendError(socket, requestId, QStringLiteral("client_gone"), QStringLiteral("client not found"));
            return;
        }
        iterator->allEvents = parameters.value(QStringLiteral("all")).toBool(false);
        iterator->eventPrefixes.clear();
        for (const QJsonValue& value : parameters.value(QStringLiteral("prefixes")).toArray()) {
            iterator->eventPrefixes.append(value.toString());
        }
        sendSuccess(socket, requestId, QJsonObject{
            {QStringLiteral("all"), iterator->allEvents},
            {QStringLiteral("prefixes"), QJsonArray::fromStringList(iterator->eventPrefixes)},
        });
        return;
    }

    if (command == QStringLiteral("event.history")) {
        quint64 afterSequence = 0;
        const QJsonValue afterValue = parameters.value(QStringLiteral("afterSequence"));
        const bool sequenceOk = afterValue.isUndefined()
            || parseUnsignedInteger(afterValue, afterSequence);
        if (!sequenceOk) {
            sendError(socket, requestId, QStringLiteral("invalid_sequence"),
                      QStringLiteral("afterSequence must be an unsigned integer"));
            return;
        }

        const int limit = qBound(
            1,
            parameters.value(QStringLiteral("limit")).toInt(256),
            static_cast<int>(maximumEventHistory));
        QStringList prefixes;
        for (const QJsonValue& value : parameters.value(QStringLiteral("prefixes")).toArray()) {
            prefixes.append(value.toString());
        }

        QJsonArray events;
        for (const QJsonObject& event : std::as_const(m_eventHistory)) {
            bool ok = false;
            const quint64 sequence = event.value(QStringLiteral("sequence"))
                                         .toString().toULongLong(&ok);
            if (!ok || sequence <= afterSequence) {
                continue;
            }

            const QString eventName = event.value(QStringLiteral("event")).toString();
            bool matches = prefixes.isEmpty();
            for (const QString& prefix : prefixes) {
                if (eventName.startsWith(prefix)) {
                    matches = true;
                    break;
                }
            }
            if (matches) {
                events.append(event);
                if (events.size() >= limit) {
                    break;
                }
            }
        }
        sendSuccess(socket, requestId, events);
        return;
    }

    if (command == QStringLiteral("module.list")) {
        sendSuccess(socket, requestId, m_modules.list());
        return;
    }
    if (command == QStringLiteral("snippet.load")) {
        QString error;
        ModuleManager::LoadedModule* module = m_modules.loadSnippet(
            parameters.value(QStringLiteral("path")).toString(), error);
        if (module == nullptr) {
            sendError(socket, requestId, QStringLiteral("load_failed"), error);
            return;
        }
        const QJsonObject result{
            {QStringLiteral("moduleId"), QString::number(module->id)},
            {QStringLiteral("name"), module->name},
            {QStringLiteral("path"), module->path},
        };
        publishEvent(QStringLiteral("snippet.loaded"), result);
        sendSuccess(socket, requestId, result);
        return;
    }
    if (command == QStringLiteral("snippet.run")) {
        quint64 moduleId = 0;
        const bool ok = parseUnsignedInteger(
            parameters.value(QStringLiteral("moduleId")), moduleId);
        ModuleManager::LoadedModule* module = m_modules.module(ok ? moduleId : 0);
        if (module == nullptr || module->kind != ModuleManager::Kind::Snippet) {
            sendError(socket, requestId, QStringLiteral("module_not_found"),
                      QStringLiteral("snippet module was not found"));
            return;
        }

        const QString executor = parameters.value(QStringLiteral("executor")).toString(
            QStringLiteral("gui"));
        if (executor != QStringLiteral("gui")
            && executor != QStringLiteral("object")
            && executor != QStringLiteral("render")) {
            sendError(socket, requestId, QStringLiteral("invalid_executor"),
                      QStringLiteral("executor must be gui, object, or render"));
            return;
        }
        QObject* target = nullptr;
        if (executor == QStringLiteral("object")) {
            QString error;
            target = resolveObject(parameters.value(QStringLiteral("target")).toObject(), error);
            if (target == nullptr) {
                sendError(socket, requestId, QStringLiteral("object_not_found"), error);
                return;
            }
        }

        const quint64 operationId = m_nextOperationId++;
        sendSuccess(socket, requestId, QJsonObject{
            {QStringLiteral("operationId"), QString::number(operationId)},
            {QStringLiteral("moduleId"), QString::number(module->id)},
            {QStringLiteral("executor"), executor},
        });

        runSnippet(module, operationId, executor, target,
                   parameters.value(QStringLiteral("request")));
        return;
    }
    if (command == QStringLiteral("snippet.release")) {
        quint64 moduleId = 0;
        const bool ok = parseUnsignedInteger(
            parameters.value(QStringLiteral("moduleId")), moduleId);
        ModuleManager::LoadedModule* module = m_modules.module(ok ? moduleId : 0);
        if (module == nullptr || module->kind != ModuleManager::Kind::Snippet) {
            sendError(socket, requestId, QStringLiteral("module_not_found"),
                      QStringLiteral("snippet module was not found"));
            return;
        }
        // A module that declares no release is answering, not failing. The
        // caller needs to tell that apart from a release that ran and broke,
        // which is the whole reason this is a declaration and not a convention.
        if (!module->declaresRelease()) {
            sendError(socket, requestId, QStringLiteral("no_release_declared"),
                      QStringLiteral("this module declares no release entry point"));
            return;
        }

        // Release has to run where install ran: an event filter on a
        // worker-thread object comes off under that object's thread, and GL
        // teardown needs the context current.
        // Preference order: what the caller said, then where the module last
        // ran successfully, then where it last ran at all. The third exists
        // because a run that installs and then fails records no success while
        // leaving state behind, and its attempt is the only place that state
        // can safely be torn down from. Reported so a caller can tell which
        // was used rather than having to assume.
        const bool executorGiven = parameters.contains(QStringLiteral("executor"));
        QString executor;
        QString executorSource;
        if (executorGiven) {
            executor = parameters.value(QStringLiteral("executor")).toString();
            executorSource = QStringLiteral("request");
        } else if (module->hadSuccessfulRun) {
            executor = module->lastExecutor;
            executorSource = QStringLiteral("recorded");
        } else if (module->hadAttemptedRun) {
            executor = module->lastAttemptedExecutor;
            executorSource = QStringLiteral("attempted");
        } else {
            sendError(socket, requestId, QStringLiteral("no_recorded_executor"),
                      QStringLiteral("this module has never been run, so it cannot have "
                                     "installed anything and there is no executor to "
                                     "release under; pass one explicitly to try anyway"));
            return;
        }
        if (executor != QStringLiteral("gui")
            && executor != QStringLiteral("object")
            && executor != QStringLiteral("render")) {
            sendError(socket, requestId, QStringLiteral("invalid_executor"),
                      QStringLiteral("executor must be gui, object, or render"));
            return;
        }

        QObject* target = nullptr;
        if (executor == QStringLiteral("object")) {
            if (parameters.contains(QStringLiteral("target"))) {
                QString error;
                target = resolveObject(parameters.value(QStringLiteral("target")).toObject(),
                                       error);
                if (target == nullptr) {
                    sendError(socket, requestId, QStringLiteral("object_not_found"), error);
                    return;
                }
            } else {
                // Follows whichever record supplied the executor, so an
                // attempted release carries the target that attempt used. A
                // recorded target can be gone by now: say so rather than
                // quietly running the release somewhere else, because where it
                // runs is the caller's decision to make.
                target = executorSource == QStringLiteral("attempted")
                    ? module->lastAttemptedTarget.data()
                    : module->lastTarget.data();
                if (target == nullptr) {
                    sendError(socket, requestId, QStringLiteral("recorded_target_gone"),
                              QStringLiteral("the object this module last %1 on no longer "
                                             "exists; pass a target explicitly")
                                  .arg(executorSource == QStringLiteral("attempted")
                                           ? QStringLiteral("attempted to run")
                                           : QStringLiteral("ran")));
                    return;
                }
            }
        }

        const quint64 operationId = m_nextOperationId++;
        sendSuccess(socket, requestId, QJsonObject{
            {QStringLiteral("operationId"), QString::number(operationId)},
            {QStringLiteral("moduleId"), QString::number(module->id)},
            {QStringLiteral("executor"), executor},
            {QStringLiteral("executorSource"), executorSource},
        });

        runSnippet(module, operationId, executor, target,
                   parameters.value(QStringLiteral("request")), SnippetEntry::Release);
        return;
    }
    if (command == QStringLiteral("stash.list")) {
        sendSuccess(socket, requestId, QJsonObject{
            {QStringLiteral("entries"), stashEntries()},
        });
        return;
    }
    if (command == QStringLiteral("stash.get")) {
        const QString key = parameters.value(QStringLiteral("key")).toString();
        QMutexLocker lock(&m_stashMutex);
        const auto found = m_stash.constFind(key);
        if (found == m_stash.constEnd()) {
            lock.unlock();
            sendError(socket, requestId, QStringLiteral("stash_key_not_found"),
                      QStringLiteral("no stash entry under that key"));
            return;
        }
        const QJsonObject result{
            {QStringLiteral("key"), key},
            {QStringLiteral("size"), static_cast<qint64>(found->bytes.size())},
            {QStringLiteral("monotonicNs"), QString::number(found->monotonicNs)},
            {QStringLiteral("moduleId"), QString::number(found->moduleId)},
            {QStringLiteral("base64"), QString::fromLatin1(found->bytes.toBase64())},
        };
        lock.unlock();
        sendSuccess(socket, requestId, result);
        return;
    }
    if (command == QStringLiteral("stash.drop")) {
        const QString key = parameters.value(QStringLiteral("key")).toString();
        sendSuccess(socket, requestId, QJsonObject{
            {QStringLiteral("key"), key},
            {QStringLiteral("dropped"), stashDrop(key)},
        });
        return;
    }
    if (command == QStringLiteral("patch.load")) {
        QString error;
        ModuleManager::LoadedModule* module = m_modules.loadCubePatch(
            parameters.value(QStringLiteral("path")).toString(), error);
        if (module == nullptr) {
            sendError(socket, requestId, QStringLiteral("load_failed"), error);
            return;
        }
        const QJsonObject result{
            {QStringLiteral("moduleId"), QString::number(module->id)},
            {QStringLiteral("name"), module->name},
            {QStringLiteral("path"), module->path},
        };
        publishEvent(QStringLiteral("patch.loaded"), result);
        sendSuccess(socket, requestId, result);
        return;
    }
    if (command == QStringLiteral("patch.activate")) {
        quint64 moduleId = 0;
        const bool ok = parseUnsignedInteger(
            parameters.value(QStringLiteral("moduleId")), moduleId);
        if (!ok) {
            sendError(socket, requestId, QStringLiteral("invalid_module_id"),
                      QStringLiteral("moduleId must be an unsigned integer"));
            return;
        }
        const QString mode = parameters.value(QStringLiteral("mode")).toString(
            QStringLiteral("dispatch"));
        if (mode != QStringLiteral("dispatch") && mode != QStringLiteral("entry")) {
            sendError(socket, requestId, QStringLiteral("invalid_patch_mode"),
                      QStringLiteral("mode must be dispatch or entry"));
            return;
        }
        QString error;
        const bool activated = mode == QStringLiteral("entry")
            ? m_modules.activateEntryPatch(moduleId, error)
            : m_modules.activateDispatchPatch(moduleId, error);
        if (!activated) {
            sendError(socket, requestId, QStringLiteral("patch_failed"), error);
            return;
        }
        const QJsonObject result = m_modules.patchStatus();
        publishEvent(QStringLiteral("patch.activated"), result);
        sendSuccess(socket, requestId, result);
        return;
    }
    if (command == QStringLiteral("patch.rollback")) {
        QString error;
        if (!m_modules.rollback(error)) {
            sendError(socket, requestId, QStringLiteral("rollback_failed"), error);
            return;
        }
        const QJsonObject result = m_modules.patchStatus();
        publishEvent(QStringLiteral("patch.rolledBack"), result);
        sendSuccess(socket, requestId, result);
        return;
    }
    if (command == QStringLiteral("patch.status")) {
        sendSuccess(socket, requestId, m_modules.patchStatus());
        return;
    }

    if (command == QStringLiteral("symbol.resolve")) {
        const QByteArray name = parameters.value(QStringLiteral("name")).toString().toUtf8();
        ::dlerror();
        void* address = ::dlsym(RTLD_DEFAULT, name.constData());
        const char* loaderError = ::dlerror();
        if (loaderError != nullptr) {
            sendError(socket, requestId, QStringLiteral("symbol_not_found"),
                      QString::fromLocal8Bit(loaderError));
            return;
        }
        sendSuccess(socket, requestId, QJsonObject{
            {QStringLiteral("name"), QString::fromUtf8(name)},
            {QStringLiteral("address"), pointerString(address)},
        });
        return;
    }
    if (command == QStringLiteral("unsafe.status")) {
        sendSuccess(socket, requestId, QJsonObject{
            {QStringLiteral("enabled"), m_unsafeEnabled},
            {QStringLiteral("environment"), QStringLiteral("QT_RUNTIME_AGENT_UNSAFE=1")},
        });
        return;
    }
    if (command == QStringLiteral("unsafe.memory.read")) {
        if (!m_unsafeEnabled) {
            sendError(socket, requestId, QStringLiteral("unsafe_disabled"),
                      QStringLiteral("start with QT_RUNTIME_AGENT_UNSAFE=1"));
            return;
        }
        quintptr address = 0;
        const qsizetype size = parameters.value(QStringLiteral("size")).toInt();
        if (!parseAddress(parameters.value(QStringLiteral("address")), address)
            || size <= 0 || size > maximumUnsafeTransferBytes) {
            sendError(socket, requestId, QStringLiteral("invalid_range"),
                      QStringLiteral("use a hex address string and size in 1..65536"));
            return;
        }
        QByteArray data(size, Qt::Uninitialized);
        iovec local{data.data(), static_cast<std::size_t>(size)};
        iovec remote{reinterpret_cast<void*>(address), static_cast<std::size_t>(size)};
        const ssize_t transferred = ::process_vm_readv(
            ::getpid(), &local, 1, &remote, 1, 0);
        if (transferred < 0) {
            sendError(socket, requestId, QStringLiteral("memory_read_failed"),
                      errnoText("process_vm_readv"));
            return;
        }
        data.truncate(static_cast<qsizetype>(transferred));
        sendSuccess(socket, requestId, QJsonObject{
            {QStringLiteral("address"), pointerString(reinterpret_cast<void*>(address))},
            {QStringLiteral("bytesRead"), static_cast<qint64>(transferred)},
            {QStringLiteral("base64"), QString::fromLatin1(data.toBase64())},
        });
        return;
    }
    if (command == QStringLiteral("unsafe.memory.write")) {
        if (!m_unsafeEnabled) {
            sendError(socket, requestId, QStringLiteral("unsafe_disabled"),
                      QStringLiteral("start with QT_RUNTIME_AGENT_UNSAFE=1"));
            return;
        }
        quintptr address = 0;
        const QByteArray data = QByteArray::fromBase64(
            parameters.value(QStringLiteral("base64")).toString().toLatin1());
        if (!parseAddress(parameters.value(QStringLiteral("address")), address)
            || data.isEmpty() || data.size() > maximumUnsafeTransferBytes) {
            sendError(socket, requestId, QStringLiteral("invalid_range"),
                      QStringLiteral("use a hex address string and 1..65536 base64 bytes"));
            return;
        }
        iovec local{const_cast<char*>(data.constData()), static_cast<std::size_t>(data.size())};
        iovec remote{reinterpret_cast<void*>(address), static_cast<std::size_t>(data.size())};
        const ssize_t transferred = ::process_vm_writev(
            ::getpid(), &local, 1, &remote, 1, 0);
        if (transferred < 0) {
            sendError(socket, requestId, QStringLiteral("memory_write_failed"),
                      errnoText("process_vm_writev"));
            return;
        }
        sendSuccess(socket, requestId, QJsonObject{
            {QStringLiteral("address"), pointerString(reinterpret_cast<void*>(address))},
            {QStringLiteral("bytesWritten"), static_cast<qint64>(transferred)},
        });
        return;
    }
    if (command == QStringLiteral("unsafe.crash")) {
        if (!m_unsafeEnabled) {
            sendError(socket, requestId, QStringLiteral("unsafe_disabled"),
                      QStringLiteral("start with QT_RUNTIME_AGENT_UNSAFE=1"));
            return;
        }
        sendSuccess(socket, requestId, QStringLiteral("crashing now"));
        socket->flush();
        volatile int* pointer = nullptr;
        *pointer = 1;
        return;
    }

    if (command == QStringLiteral("process.quit")) {
        sendSuccess(socket, requestId, true);
        socket->flush();
        QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
        return;
    }

    sendError(socket, requestId, QStringLiteral("unknown_command"), command);
}

void RuntimeAgent::sendSuccess(QLocalSocket* socket,
                               const QJsonValue& requestId,
                               const QJsonValue& result)
{
    sendObject(socket, QJsonObject{
        {QStringLiteral("id"), requestId},
        {QStringLiteral("ok"), true},
        {QStringLiteral("result"), result},
    });
}

void RuntimeAgent::sendError(QLocalSocket* socket,
                             const QJsonValue& requestId,
                             const QString& code,
                             const QString& message)
{
    sendObject(socket, QJsonObject{
        {QStringLiteral("id"), requestId},
        {QStringLiteral("ok"), false},
        {QStringLiteral("error"), QJsonObject{
            {QStringLiteral("code"), code},
            {QStringLiteral("message"), message},
        }},
    });
}

void RuntimeAgent::sendObject(QLocalSocket* socket, const QJsonObject& object)
{
    if (socket == nullptr || socket->state() == QLocalSocket::UnconnectedState) {
        return;
    }
    socket->write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    socket->write("\n");
}

QObject* RuntimeAgent::resolveObject(const QJsonObject& parameters, QString& error) const
{
    if (parameters.contains(QStringLiteral("objectName"))) {
        const QString name = parameters.value(QStringLiteral("objectName")).toString();
        if (QObject* object = m_registry.byObjectName(name); object != nullptr) {
            return object;
        }
        error = QStringLiteral("no QObject named %1").arg(name);
        return nullptr;
    }
    if (parameters.contains(QStringLiteral("id"))) {
        quint64 id = 0;
        const bool ok = parseUnsignedInteger(parameters.value(QStringLiteral("id")), id);
        if (ok) {
            if (QObject* object = m_registry.byId(id); object != nullptr) {
                return object;
            }
        }
        error = QStringLiteral("no live QObject with id %1").arg(id);
        return nullptr;
    }
    error = QStringLiteral("objectName or id is required");
    return nullptr;
}

QJsonObject RuntimeAgent::cubeState() const
{
    return QJsonObject{
        {QStringLiteral("angleDegrees"), m_cube->angleDegrees()},
        {QStringLiteral("angularVelocity"), m_cube->angularVelocity()},
        {QStringLiteral("running"), m_cube->isRunning()},
        {QStringLiteral("wireframe"), m_cube->isWireframe()},
        {QStringLiteral("frameIndex"), QString::number(m_cube->frameIndex())},
        {QStringLiteral("activePatch"), m_cube->activePatch()},
        {QStringLiteral("glVendor"), m_cube->glVendor()},
        {QStringLiteral("glRenderer"), m_cube->glRenderer()},
        {QStringLiteral("glVersion"), m_cube->glVersion()},
        {QStringLiteral("address"), pointerString(m_cube)},
    };
}

QJsonObject RuntimeAgent::hello() const
{
#ifdef NDEBUG
    constexpr bool optimizedBuild = true;
#else
    constexpr bool optimizedBuild = false;
#endif
    return QJsonObject{
        {QStringLiteral("name"), QStringLiteral("qt-runtime-agent-demo")},
        {QStringLiteral("protocolVersion"), 1},
        {QStringLiteral("agentAbi"), QStringLiteral("0x%1").arg(RUNTIME_AGENT_ABI_V1, 8, 16, QLatin1Char('0'))},
        {QStringLiteral("cubePatchAbi"), QStringLiteral("0x%1").arg(CUBE_STEP_ABI_V1, 8, 16, QLatin1Char('0'))},
        {QStringLiteral("pid"), QCoreApplication::applicationPid()},
        {QStringLiteral("uid"), static_cast<int>(::getuid())},
        {QStringLiteral("socket"), m_socketName},
        {QStringLiteral("qtVersion"), QString::fromLatin1(qVersion())},
        {QStringLiteral("compiler"), QString::fromLatin1(__VERSION__)},
        {QStringLiteral("optimizedBuild"), optimizedBuild},
        {QStringLiteral("unsafeEnabled"), m_unsafeEnabled},
        {QStringLiteral("sourceDir"), QStringLiteral(DEMO_SOURCE_DIR)},
        {QStringLiteral("buildDir"), QStringLiteral(DEMO_BUILD_DIR)},
        {QStringLiteral("buildType"), QStringLiteral(DEMO_BUILD_TYPE)},
        {QStringLiteral("cube"), cubeState()},
        {QStringLiteral("patch"), m_modules.patchStatus()},
    };
}

QJsonArray RuntimeAgent::commandList() const
{
    const QStringList commands{
        QStringLiteral("hello"), QStringLiteral("help"),
        QStringLiteral("cube.state"), QStringLiteral("cube.pause"),
        QStringLiteral("cube.resume"), QStringLiteral("cube.reset"),
        QStringLiteral("cube.speed"), QStringLiteral("cube.wireframe"),
        QStringLiteral("cube.capture"),
        QStringLiteral("object.tree"), QStringLiteral("object.list"),
        QStringLiteral("object.describe"), QStringLiteral("object.get"),
        QStringLiteral("object.set"), QStringLiteral("object.invoke"),
        QStringLiteral("action.trigger"), QStringLiteral("widget.click"),
        QStringLiteral("event.subscribe"), QStringLiteral("event.history"),
        QStringLiteral("module.list"),
        QStringLiteral("snippet.load"), QStringLiteral("snippet.run"),
        QStringLiteral("snippet.release"),
        QStringLiteral("stash.list"), QStringLiteral("stash.get"),
        QStringLiteral("stash.drop"),
        QStringLiteral("patch.load"), QStringLiteral("patch.activate"),
        QStringLiteral("patch.rollback"), QStringLiteral("patch.status"),
        QStringLiteral("symbol.resolve"), QStringLiteral("unsafe.status"),
        QStringLiteral("unsafe.memory.read"), QStringLiteral("unsafe.memory.write"),
        QStringLiteral("unsafe.crash"), QStringLiteral("process.quit"),
    };
    return QJsonArray::fromStringList(commands);
}

RuntimeAgent::ModuleContext* RuntimeAgent::contextForModule(const quint64 moduleId)
{
    auto found = m_moduleContexts.find(moduleId);
    if (found == m_moduleContexts.end()) {
        auto context = std::make_unique<ModuleContext>();
        context->agent = this;
        context->moduleId = moduleId;
        found = m_moduleContexts.emplace(moduleId, std::move(context)).first;
    }
    return found->second.get();
}

RuntimeAgent* RuntimeAgent::agentOf(void* agentContext)
{
    return static_cast<ModuleContext*>(agentContext)->agent;
}

void RuntimeAgent::runSnippet(ModuleManager::LoadedModule* module,
                              const quint64 operationId,
                              const QString& executor,
                              QObject* target,
                              const QJsonValue& request,
                              const SnippetEntry entry)
{
    auto invocation = std::make_shared<SnippetInvocation>();
    invocation->agent = this;
    invocation->operationId = operationId;
    invocation->entry = entry;
    invocation->executor = executor;
    invocation->target = target;
    if (request.isObject()) {
        invocation->requestJson = QJsonDocument(request.toObject()).toJson(QJsonDocument::Compact);
    } else if (request.isArray()) {
        invocation->requestJson = QJsonDocument(request.toArray()).toJson(QJsonDocument::Compact);
    } else {
        invocation->requestJson = "{}";
    }

    invocation->host = RuntimeAgentHostV1{
        RUNTIME_AGENT_ABI_V1,
        sizeof(RuntimeAgentHostV1),
        // Not the agent itself: the module's own context, so that a callback
        // made later from a draw hook or a menu handler still identifies the
        // module it came from.
        contextForModule(module->id),
        invocation.get(),
        &RuntimeAgent::hostLog,
        &RuntimeAgent::hostEmitEvent,
        &RuntimeAgent::hostFindQObject,
        &RuntimeAgent::hostFindSymbol,
        &RuntimeAgent::hostRequestJson,
        &RuntimeAgent::hostCompleteJson,
        &RuntimeAgent::hostFail,
        &RuntimeAgent::hostMonotonicTimeNs,
        &RuntimeAgent::hostStashPut,
        &RuntimeAgent::hostStashGet,
        &RuntimeAgent::hostStashDrop,
        &RuntimeAgent::hostStashList,
    };

    publishEvent(QStringLiteral("operation.started"), QJsonObject{
        {QStringLiteral("operationId"), QString::number(invocation->operationId)},
        {QStringLiteral("kind"), entry == SnippetEntry::Release
            ? QStringLiteral("snippetRelease") : QStringLiteral("snippet")},
        {QStringLiteral("moduleId"), QString::number(module->id)},
        {QStringLiteral("name"), module->name},
        {QStringLiteral("executor"), executor},
    });

    auto callback = [this, module, invocation] {
        executeSnippet(module, invocation);
    };

    bool scheduled = true;
    if (executor == QStringLiteral("render")) {
        m_cube->enqueueRenderCallback(std::move(callback));
    } else if (executor == QStringLiteral("object") && target != nullptr) {
        scheduled = QMetaObject::invokeMethod(target, std::move(callback), Qt::QueuedConnection);
    } else {
        scheduled = QMetaObject::invokeMethod(m_window, std::move(callback), Qt::QueuedConnection);
    }

    if (!scheduled) {
        invocation->error = QStringLiteral("Qt rejected the queued snippet invocation");
        invocation->completed = true;
        finishSnippet(module, invocation);
    }
}

void RuntimeAgent::executeSnippet(ModuleManager::LoadedModule* module,
                                  const std::shared_ptr<SnippetInvocation>& invocation)
{
    // Recorded before the call, and for every attempt: this is the only witness
    // to where an install ran when the run that made it went on to fail.
    if (invocation->entry == SnippetEntry::Run) {
        module->lastAttemptedExecutor = invocation->executor;
        module->lastAttemptedTarget = invocation->target;
        module->hadAttemptedRun = true;
    }

    try {
        if (invocation->entry == SnippetEntry::Release) {
            module->snippet->release(&invocation->host);
        } else {
            module->snippet->run(&invocation->host);
        }
    } catch (const std::exception& exception) {
        invocation->error = QString::fromUtf8(exception.what());
    } catch (...) {
        invocation->error = QStringLiteral("snippet threw a non-standard C++ exception");
    }

    if (!invocation->completed && invocation->error.isEmpty()) {
        invocation->error = QStringLiteral(
            "snippet returned without calling complete_json() or fail()");
    }
    finishSnippet(module, invocation);
}

void RuntimeAgent::finishSnippet(
    ModuleManager::LoadedModule* module,
    const std::shared_ptr<SnippetInvocation>& invocation)
{
    QJsonObject event{
        {QStringLiteral("operationId"), QString::number(invocation->operationId)},
        {QStringLiteral("kind"), invocation->entry == SnippetEntry::Release
            ? QStringLiteral("snippetRelease") : QStringLiteral("snippet")},
        {QStringLiteral("moduleId"), QString::number(module->id)},
        {QStringLiteral("name"), module->name},
    };
    if (invocation->error.isEmpty()) {
        // Only a run that got this far is worth remembering as the way to reach
        // this module. Recording a failed attempt would send its release to the
        // executor that already turned it away.
        if (invocation->entry == SnippetEntry::Run) {
            module->lastExecutor = invocation->executor;
            module->lastTarget = invocation->target;
            module->hadSuccessfulRun = true;
        }
        event.insert(QStringLiteral("outcome"), QStringLiteral("completed"));
        event.insert(QStringLiteral("result"), parseSnippetResult(invocation->resultJson));
    } else {
        event.insert(QStringLiteral("outcome"), QStringLiteral("failed"));
        event.insert(QStringLiteral("error"), invocation->error);
    }
    publishEvent(QStringLiteral("operation.finished"), event);
}

QJsonValue RuntimeAgent::parseSnippetResult(const QByteArray& json) const
{
    if (json.isEmpty()) {
        return QJsonObject{};
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError) {
        return QJsonObject{
            {QStringLiteral("invalidJson"), QString::fromUtf8(json)},
            {QStringLiteral("parseError"), error.errorString()},
        };
    }
    return document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object());
}

bool RuntimeAgent::clientWantsEvent(const ClientState& state, const QString& eventName) const
{
    if (state.allEvents) {
        return true;
    }
    for (const QString& prefix : state.eventPrefixes) {
        if (eventName.startsWith(prefix)) {
            return true;
        }
    }
    return false;
}

void RuntimeAgent::hostLog(void* agentContext,
                           const std::int32_t level,
                           const char* message)
{
    auto* agent = agentOf(agentContext);
    const QString text = QString::fromUtf8(message != nullptr ? message : "");
    switch (level) {
    case RUNTIME_AGENT_LOG_ERROR: qCritical().noquote() << "[snippet]" << text; break;
    case RUNTIME_AGENT_LOG_WARNING: qWarning().noquote() << "[snippet]" << text; break;
    case RUNTIME_AGENT_LOG_DEBUG: qDebug().noquote() << "[snippet]" << text; break;
    default: qInfo().noquote() << "[snippet]" << text; break;
    }
    agent->publishEvent(QStringLiteral("snippet.log"), QJsonObject{
        {QStringLiteral("level"), level},
        {QStringLiteral("message"), text},
    });
}

void RuntimeAgent::hostEmitEvent(void* agentContext,
                                 const char* name,
                                 const char* objectJson)
{
    auto* agent = agentOf(agentContext);
    const QString eventName = QString::fromUtf8(name != nullptr ? name : "snippet.event");
    agent->publishEvent(eventName,
                        parseObjectOrRaw(QByteArray(objectJson != nullptr ? objectJson : "{}")));
}

void* RuntimeAgent::hostFindQObject(void* agentContext, const char* objectName)
{
    auto* agent = agentOf(agentContext);
    return agent->findObject(QString::fromUtf8(objectName != nullptr ? objectName : ""));
}

void* RuntimeAgent::hostFindSymbol(void*, const char* symbolName)
{
    ::dlerror();
    return ::dlsym(RTLD_DEFAULT, symbolName != nullptr ? symbolName : "");
}

const char* RuntimeAgent::hostRequestJson(void* invocationContext)
{
    // A module that kept a by-value copy of the host for a callback outliving
    // its invocation has invocation_context cleared, as the ABI tells it to.
    // Calling this from there is a mistake, but it is a mistake that would
    // otherwise dereference null inside the host, so it is reported instead.
    if (invocationContext == nullptr) {
        qWarning("runtime-agent: %s called outside an invocation; "
                 "invocation_context is null", __func__);
        return "{}";
    }
    auto* invocation = static_cast<SnippetInvocation*>(invocationContext);
    return invocation->requestJson.constData();
}

void RuntimeAgent::hostCompleteJson(void* invocationContext, const char* resultJson)
{
    // A module that kept a by-value copy of the host for a callback outliving
    // its invocation has invocation_context cleared, as the ABI tells it to.
    // Calling this from there is a mistake, but it is a mistake that would
    // otherwise dereference null inside the host, so it is reported instead.
    if (invocationContext == nullptr) {
        qWarning("runtime-agent: %s called outside an invocation; "
                 "invocation_context is null", __func__);
        return;
    }
    auto* invocation = static_cast<SnippetInvocation*>(invocationContext);
    if (invocation->completed) {
        invocation->error = QStringLiteral("snippet completed more than once");
        return;
    }
    invocation->resultJson = QByteArray(resultJson != nullptr ? resultJson : "{}");
    invocation->completed = true;
}

void RuntimeAgent::hostFail(void* invocationContext, const char* error)
{
    // A module that kept a by-value copy of the host for a callback outliving
    // its invocation has invocation_context cleared, as the ABI tells it to.
    // Calling this from there is a mistake, but it is a mistake that would
    // otherwise dereference null inside the host, so it is reported instead.
    if (invocationContext == nullptr) {
        qWarning("runtime-agent: %s called outside an invocation; "
                 "invocation_context is null", __func__);
        return;
    }
    auto* invocation = static_cast<SnippetInvocation*>(invocationContext);
    if (invocation->completed) {
        invocation->error = QStringLiteral("snippet completed more than once");
        return;
    }
    invocation->error = QString::fromUtf8(error != nullptr ? error : "snippet failed");
    invocation->completed = true;
}

std::uint64_t RuntimeAgent::hostMonotonicTimeNs(void* agentContext)
{
    auto* agent = agentOf(agentContext);
    return static_cast<std::uint64_t>(agent->m_monotonicClock.nsecsElapsed());
}

std::int32_t RuntimeAgent::hostStashPut(void* agentContext,
                                        const char* key,
                                        const void* bytes,
                                        const std::int64_t size,
                                        const std::int32_t overwrite)
{
    if (key == nullptr || *key == '\0' || size < 0 || (bytes == nullptr && size > 0)) {
        return -2;
    }
    auto* context = static_cast<ModuleContext*>(agentContext);
    auto* agent = context->agent;
    const QString name = QString::fromUtf8(key);

    QMutexLocker lock(&agent->m_stashMutex);
    const bool existed = agent->m_stash.contains(name);
    if (existed && overwrite == 0) {
        return -1;
    }
    StashEntry entry;
    entry.bytes = QByteArray(static_cast<const char*>(bytes), static_cast<qsizetype>(size));
    entry.monotonicNs = static_cast<quint64>(agent->m_monotonicClock.nsecsElapsed());
    entry.moduleId = context->moduleId;
    agent->m_stash.insert(name, entry);
    return existed ? 1 : 0;
}

std::int64_t RuntimeAgent::hostStashGet(void* agentContext,
                                        const char* key,
                                        void* buffer,
                                        const std::int64_t capacity)
{
    if (key == nullptr || capacity < 0 || (buffer == nullptr && capacity > 0)) {
        return -1;
    }
    auto* agent = agentOf(agentContext);

    QMutexLocker lock(&agent->m_stashMutex);
    const auto found = agent->m_stash.constFind(QString::fromUtf8(key));
    if (found == agent->m_stash.constEnd()) {
        return -1;
    }
    const QByteArray& bytes = found->bytes;
    const std::int64_t total = bytes.size();
    if (capacity > 0) {
        std::memcpy(buffer, bytes.constData(), static_cast<std::size_t>(std::min(total, capacity)));
    }
    return total;
}

std::int32_t RuntimeAgent::hostStashDrop(void* agentContext, const char* key)
{
    if (key == nullptr) {
        return 0;
    }
    return agentOf(agentContext)->stashDrop(QString::fromUtf8(key)) ? 1 : 0;
}

std::int64_t RuntimeAgent::hostStashList(void* agentContext,
                                         char* buffer,
                                         const std::int64_t capacity)
{
    if (capacity < 0 || (buffer == nullptr && capacity > 0)) {
        return -1;
    }
    auto* agent = agentOf(agentContext);
    const QByteArray json =
        QJsonDocument(agent->stashEntries()).toJson(QJsonDocument::Compact);
    const std::int64_t total = json.size();
    if (capacity > 0) {
        std::memcpy(buffer, json.constData(), static_cast<std::size_t>(std::min(total, capacity)));
    }
    return total;
}

QJsonArray RuntimeAgent::stashEntries() const
{
    QMutexLocker lock(&m_stashMutex);
    QJsonArray entries;
    for (auto it = m_stash.constBegin(); it != m_stash.constEnd(); ++it) {
        entries.append(QJsonObject{
            {QStringLiteral("key"), it.key()},
            {QStringLiteral("size"), static_cast<qint64>(it->bytes.size())},
            {QStringLiteral("monotonicNs"), QString::number(it->monotonicNs)},
            {QStringLiteral("moduleId"), QString::number(it->moduleId)},
        });
    }
    return entries;
}

bool RuntimeAgent::stashDrop(const QString& key)
{
    QMutexLocker lock(&m_stashMutex);
    return m_stash.remove(key) > 0;
}
