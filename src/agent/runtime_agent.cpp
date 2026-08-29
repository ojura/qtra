#include "agent/runtime_agent.h"

#include "agent/build_id.h"
#include "agent/errno_text.h"

#include <QAbstractButton>
#include <QAction>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <exception>
#include <mutex>
#include <utility>

#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

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

QJsonObject parseObjectOrRaw(const QByteArray& json)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error == QJsonParseError::NoError && document.isObject()) {
        return document.object();
    }
    return QJsonObject{{QStringLiteral("raw"), QString::fromUtf8(json)}};
}

QString failedCall(const char* operation, const int errorNumber)
{
    return QStringLiteral("%1 failed: %2")
        .arg(QString::fromLatin1(operation),
             QString::fromStdString(runtime_agent::errnoText(errorNumber)));
}

QByteArray compactJsonValue(const QJsonValue& value)
{
    if (value.isUndefined()) {
        return "{}";
    }
    if (value.isObject()) {
        return QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    }
    if (value.isArray()) {
        return QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);
    }

    const QByteArray wrapped =
        QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return wrapped.mid(1, wrapped.size() - 2);
}

} // namespace

struct RuntimeAgent::CallbackLifetime {
    explicit CallbackLifetime(RuntimeAgent* owner)
        : agent(owner)
    {
    }

    RuntimeAgent* acquire()
    {
        std::lock_guard lock(mutex);
        if (stopping || agent == nullptr) {
            return nullptr;
        }
        ++active;
        return agent;
    }

    void release()
    {
        std::lock_guard lock(mutex);
        Q_ASSERT(active > 0);
        --active;
        if (stopping && active == 0) {
            idle.notify_all();
        }
    }

    void stop()
    {
        std::unique_lock lock(mutex);
        stopping = true;
        agent = nullptr;
        while (active != 0) {
            if (idle.wait_for(lock, std::chrono::seconds(5))
                == std::cv_status::timeout) {
                qWarning().noquote()
                    << "runtime-agent: shutdown is waiting for" << active
                    << "native callback or executor invocation(s) still using agent state";
            }
        }
    }

    std::mutex mutex;
    std::condition_variable idle;
    RuntimeAgent* agent = nullptr;
    std::size_t active = 0;
    bool stopping = false;
};

class RuntimeAgent::CallbackLease final {
public:
    CallbackLease() = default;

    explicit CallbackLease(std::shared_ptr<CallbackLifetime> lifetime)
        : m_lifetime(std::move(lifetime))
        , m_agent(m_lifetime != nullptr ? m_lifetime->acquire() : nullptr)
    {
        if (m_agent == nullptr) {
            m_lifetime.reset();
        }
    }

    CallbackLease(const CallbackLease&) = delete;
    CallbackLease& operator=(const CallbackLease&) = delete;

    CallbackLease(CallbackLease&& other) noexcept
        : m_lifetime(std::move(other.m_lifetime))
        , m_agent(std::exchange(other.m_agent, nullptr))
    {
    }

    CallbackLease& operator=(CallbackLease&& other) noexcept
    {
        if (this == &other) {
            return *this;
        }
        reset();
        m_lifetime = std::move(other.m_lifetime);
        m_agent = std::exchange(other.m_agent, nullptr);
        return *this;
    }

    ~CallbackLease()
    {
        reset();
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return m_agent != nullptr;
    }

    [[nodiscard]] RuntimeAgent* get() const noexcept
    {
        return m_agent;
    }

    RuntimeAgent* operator->() const noexcept
    {
        return m_agent;
    }

private:
    void reset()
    {
        if (m_lifetime != nullptr) {
            m_lifetime->release();
            m_lifetime.reset();
        }
        m_agent = nullptr;
    }

    std::shared_ptr<CallbackLifetime> m_lifetime;
    RuntimeAgent* m_agent = nullptr;
};

RuntimeAgent::Client::Client(RuntimeAgent* owner, QLocalSocket* socket)
    : m_owner(owner)
    , m_lifetime(owner != nullptr ? owner->m_callbackLifetime : nullptr)
    , m_socket(socket)
{
}

RuntimeAgent::RuntimeAgent(QObject* root,
                           QString socketName,
                           QObject* parent)
    : QObject(parent)
    , m_modules(runtime_agent::ModuleRegistry::instance())
    , m_callbackLifetime(std::make_shared<CallbackLifetime>(this))
    , m_socketName(std::move(socketName))
    , m_registry(root, this)
    , m_unsafeEnabled(qEnvironmentVariableIntValue("QT_RUNTIME_AGENT_UNSAFE") == 1)
{
    setObjectName(QStringLiteral("runtimeAgent"));
    m_monotonicClock.start();
    registerCoreCommands();

    if (root == nullptr) {
        return;
    }
    for (QAction* action : root->findChildren<QAction*>()) {
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
    // Stop new native callbacks and executor work, then wait until anything that
    // already entered has stopped using agent-owned state.
    m_callbackLifetime->stop();

    // Accepted sockets are QObject children of this agent, so QObject deletes
    // them after the agent's members. Each socket aborts on the way out and emits
    // disconnected, which would reach removeClient after m_clients was gone. Cut
    // those connections while every member is still alive.
    for (auto iterator = m_clients.cbegin(); iterator != m_clients.cend(); ++iterator) {
        disconnect(iterator.key(), nullptr, this, nullptr);
    }
    m_clients.clear();

    m_listenNotifier.reset();
    if (m_listenSocket >= 0) {
        (void)::close(m_listenSocket);
        m_listenSocket = -1;
    }

    const std::optional<BoundSocketIdentity> boundSocket = m_boundSocket;
    m_boundSocket.reset();

    // The pathname may have been unlinked and reused while this server was
    // alive. Remove it only when it still names the socket this instance bound.
    if (boundSocket.has_value()) {
        const QByteArray encodedPath = QFile::encodeName(boundSocket->path);
        struct stat current {};
        if (::lstat(encodedPath.constData(), &current) == 0) {
            const bool sameSocket = S_ISSOCK(current.st_mode)
                && static_cast<std::uint64_t>(current.st_dev) == boundSocket->device
                && static_cast<std::uint64_t>(current.st_ino) == boundSocket->inode;
            if (sameSocket && ::unlink(encodedPath.constData()) != 0) {
                qWarning().noquote()
                    << "runtime-agent: could not remove its socket during shutdown:"
                    << failedCall("unlink(runtime-agent socket)", errno);
            }
        } else if (errno != ENOENT) {
            qWarning().noquote()
                << "runtime-agent: could not inspect its socket during shutdown:"
                << failedCall("lstat(runtime-agent socket)", errno);
        }
    }
}

bool RuntimeAgent::start(QString& error)
{
    if (m_socketName.isEmpty()) {
        error = QStringLiteral("socket name is empty");
        return false;
    }
    if (!QFileInfo(m_socketName).isAbsolute()) {
        error = QStringLiteral("runtime-agent socket path must be absolute: %1")
                    .arg(m_socketName);
        return false;
    }
    if (m_listenSocket >= 0) {
        error = QStringLiteral("runtime agent is already listening on %1").arg(m_socketName);
        return false;
    }

    const QByteArray encodedSocketName = QFile::encodeName(m_socketName);
    if (encodedSocketName.contains('\0')) {
        error = QStringLiteral("runtime-agent socket path contains a null byte");
        return false;
    }

    sockaddr_un address {};
    if (encodedSocketName.size() >= static_cast<qsizetype>(sizeof(address.sun_path))) {
        error = QStringLiteral("runtime-agent socket path is too long for AF_UNIX: %1")
                    .arg(m_socketName);
        return false;
    }

    // Only a filesystem socket that definitively has no listener may be removed.
    // A timeout or resource error says nothing about whether a live server owns
    // it, and unlinking a non-socket would destroy somebody else's file.
    struct stat endpointStatus {};
    if (::lstat(encodedSocketName.constData(), &endpointStatus) == 0) {
        if (!S_ISSOCK(endpointStatus.st_mode)) {
            error = QStringLiteral("runtime-agent socket path exists and is not a Unix socket: %1")
                        .arg(m_socketName);
            return false;
        }

        QLocalSocket probe;
        probe.connectToServer(m_socketName);
        if (probe.waitForConnected(100)) {
            probe.disconnectFromServer();
            error = QStringLiteral("runtime-agent socket is already in use: %1")
                        .arg(m_socketName);
            return false;
        }
        const QLocalSocket::LocalSocketError probeError = probe.error();
        if (probeError != QLocalSocket::ConnectionRefusedError
            && probeError != QLocalSocket::ServerNotFoundError) {
            error = QStringLiteral(
                        "could not prove that the existing runtime-agent socket is stale: %1")
                        .arg(probe.errorString());
            return false;
        }
        if (::unlink(encodedSocketName.constData()) != 0) {
            error = failedCall("unlink(stale runtime-agent socket)", errno);
            return false;
        }
    } else if (errno != ENOENT) {
        error = failedCall("lstat(runtime-agent socket)", errno);
        return false;
    }

    int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        error = failedCall("socket(AF_UNIX)", errno);
        return false;
    }

    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path,
                encodedSocketName.constData(),
                static_cast<std::size_t>(encodedSocketName.size()));
    const socklen_t addressLength = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + encodedSocketName.size() + 1);
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), addressLength) != 0) {
        error = failedCall("bind(runtime-agent socket)", errno);
        (void)::close(listener);
        return false;
    }

    struct stat boundStatus {};
    if (::lstat(encodedSocketName.constData(), &boundStatus) != 0) {
        error = failedCall("lstat(bound runtime-agent socket)", errno);
        (void)::close(listener);
        return false;
    }
    if (!S_ISSOCK(boundStatus.st_mode)) {
        error = QStringLiteral("the bound runtime-agent path is not a Unix socket");
        (void)::close(listener);
        return false;
    }
    const BoundSocketIdentity identity{
        m_socketName,
        static_cast<std::uint64_t>(boundStatus.st_dev),
        static_cast<std::uint64_t>(boundStatus.st_ino),
    };

    const auto removeBoundSocket = [&] {
        struct stat current {};
        if (::lstat(encodedSocketName.constData(), &current) == 0
            && S_ISSOCK(current.st_mode)
            && static_cast<std::uint64_t>(current.st_dev) == identity.device
            && static_cast<std::uint64_t>(current.st_ino) == identity.inode) {
            (void)::unlink(encodedSocketName.constData());
        }
    };

    // The node exists before listen, so setting 0600 here admits no connection
    // under broader umask-derived permissions.
    if (::chmod(encodedSocketName.constData(), S_IRUSR | S_IWUSR) != 0) {
        error = failedCall("chmod(runtime-agent socket)", errno);
        (void)::close(listener);
        removeBoundSocket();
        return false;
    }
    constexpr int listenBacklog = 50;
    if (::listen(listener, listenBacklog) != 0) {
        error = failedCall("listen(runtime-agent socket)", errno);
        (void)::close(listener);
        removeBoundSocket();
        return false;
    }

    auto notifier = std::make_unique<QSocketNotifier>(listener, QSocketNotifier::Read);
    connect(notifier.get(), &QSocketNotifier::activated,
            this, [this] { acceptConnections(); });

    m_listenSocket = listener;
    m_listenNotifier = std::move(notifier);
    m_boundSocket = identity;
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
    CallbackLease lease(m_callbackLifetime);
    if (!lease || lease.get() != this) {
        return;
    }
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this, name, data] { publishEvent(name, data); },
            Qt::QueuedConnection);
        return;
    }

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
    while (m_listenSocket >= 0) {
        const int accepted = ::accept4(
            m_listenSocket, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (accepted < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                qWarning().noquote()
                    << "runtime-agent:" << failedCall("accept4", errno);
            }
            return;
        }

        auto* socket = new QLocalSocket(this);
        if (!socket->setSocketDescriptor(
                accepted, QLocalSocket::ConnectedState, QIODevice::ReadWrite)) {
            qWarning().noquote()
                << "runtime-agent: could not adopt an accepted local socket:"
                << socket->errorString();
            (void)::close(accepted);
            delete socket;
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

// One entry per command the core answers itself, and the only place that set is
// written down. help enumerates the table instead of restating it, so a command
// cannot be reachable but undiscoverable, or listed with no handler behind it.
//
// An application's commands land in the same table through registerCommand, so
// they answer to the same dispatch and the same two refusals, and help lists
// them beside these.
void RuntimeAgent::registerCoreCommands()
{
    const auto add = [this](const char* name,
                            QStringList parameters,
                            void (RuntimeAgent::*handler)(QLocalSocket*,
                                                          const QJsonValue&,
                                                          const QJsonObject&)) {
        const bool added = registerCommand(
            QString::fromLatin1(name), std::move(parameters),
            [this, handler](Client client,
                            const QJsonValue& requestId,
                            const QJsonObject& parameters) {
                auto* socket = qobject_cast<QLocalSocket*>(client.m_socket.data());
                if (socket != nullptr) {
                    (this->*handler)(socket, requestId, parameters);
                }
            });
        Q_ASSERT_X(added, "registerCoreCommands", name);
        Q_UNUSED(added);
    };

    add("hello", {}, &RuntimeAgent::handleHello);
    add("help", {}, &RuntimeAgent::handleHelp);
    add("object.tree", {"objectName", "id", "maxDepth"}, &RuntimeAgent::handleObjectTree);
    add("object.list", {}, &RuntimeAgent::handleObjectList);
    add("object.describe", {"objectName", "id", "includeValues"}, &RuntimeAgent::handleObjectDescribe);
    add("object.get", {"objectName", "id", "property"}, &RuntimeAgent::handleObjectGet);
    add("object.set", {"objectName", "id", "property", "value"}, &RuntimeAgent::handleObjectSet);
    add("object.invoke", {"objectName", "id", "method"}, &RuntimeAgent::handleObjectInvoke);
    add("action.trigger", {"objectName", "id"}, &RuntimeAgent::handleActionTrigger);
    add("widget.click", {"objectName", "id"}, &RuntimeAgent::handleWidgetClick);
    add("event.subscribe", {"all", "prefixes"}, &RuntimeAgent::handleEventSubscribe);
    add("event.history", {"afterSequence", "limit", "prefixes"}, &RuntimeAgent::handleEventHistory);
    add("module.list", {}, &RuntimeAgent::handleModuleList);
    add("snippet.load", {"path"}, &RuntimeAgent::handleSnippetLoad);
    add("snippet.run", {"moduleId", "executor", "target", "request"}, &RuntimeAgent::handleSnippetRun);
    add("snippet.release", {"moduleId", "executor", "target", "request"}, &RuntimeAgent::handleSnippetRelease);
    add("stash.list", {}, &RuntimeAgent::handleStashList);
    add("stash.get", {"key"}, &RuntimeAgent::handleStashGet);
    add("stash.drop", {"key"}, &RuntimeAgent::handleStashDrop);
    add("symbol.resolve", {"name"}, &RuntimeAgent::handleSymbolResolve);
    add("unsafe.status", {}, &RuntimeAgent::handleUnsafeStatus);
    add("unsafe.memory.read", {"address", "size"}, &RuntimeAgent::handleUnsafeMemoryRead);
    add("unsafe.memory.write", {"address", "base64"}, &RuntimeAgent::handleUnsafeMemoryWrite);
    add("unsafe.crash", {}, &RuntimeAgent::handleUnsafeCrash);
    add("process.quit", {}, &RuntimeAgent::handleProcessQuit);
}

bool RuntimeAgent::registerCommand(QString name,
                                   QStringList parameters,
                                   CommandHandler handler)
{
    if (name.isEmpty() || !handler) {
        return false;
    }
    const bool taken = std::any_of(m_commands.begin(), m_commands.end(),
                                   [&name](const CommandEntry& entry) {
                                       return entry.name == name;
                                   });
    if (taken) {
        return false;
    }
    m_commands.push_back(
        CommandEntry{std::move(name), std::move(parameters), std::move(handler)});
    return true;
}

bool RuntimeAgent::registerExecutor(QString name, SnippetExecutor executor)
{
    // "gui" and "object" are the core's own, and the empty name is what a
    // request that says nothing falls back to, so none is an application's to
    // take.
    if (name.isEmpty() || !executor || name == QStringLiteral("gui")
        || name == QStringLiteral("object") || m_executors.contains(name)) {
        return false;
    }
    m_executors.insert(std::move(name), std::move(executor));
    return true;
}

bool RuntimeAgent::registerHelloField(QString name, std::function<QJsonValue()> value)
{
    if (name.isEmpty() || !value || m_helloFields.contains(name)
        || coreHello().contains(name)) {
        return false;
    }
    m_helloFields.insert(std::move(name), std::move(value));
    return true;
}

bool RuntimeAgent::registerPatchProvider(PatchBindHandler bind, PatchUnbindHandler unbind)
{
    if (!bind || !unbind || m_patchBind || m_patchUnbind) {
        return false;
    }
    m_patchBind = std::move(bind);
    m_patchUnbind = std::move(unbind);
    return true;
}

bool RuntimeAgent::parseUnsignedInteger(const QJsonValue& value, quint64& result)
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

void RuntimeAgent::dispatchRequest(QLocalSocket* socket,
                                   const QJsonValue& requestId,
                                   const QString& command,
                                   const QJsonObject& parameters)
{
    for (const CommandEntry& entry : m_commands) {
        if (command != entry.name) {
            continue;
        }
        // A parameter nobody reads makes a request that succeeds and does
        // nothing, and the caller cannot tell the difference from one that
        // worked. Refusing turns that into a failure the first time it is
        // sent.
        for (auto it = parameters.begin(); it != parameters.end(); ++it) {
            if (!entry.parameters.contains(it.key())) {
                sendError(socket, requestId, QStringLiteral("unknown_parameter"),
                          QStringLiteral("%1 does not read %2; it reads %3")
                              .arg(command, it.key(),
                                   entry.parameters.isEmpty()
                                       ? QStringLiteral("nothing")
                                       : entry.parameters.join(QStringLiteral(", "))));
                return;
            }
        }
        entry.handler(Client(this, socket), requestId, parameters);
        return;
    }
    sendError(socket, requestId, QStringLiteral("unknown_command"), command);
}

void RuntimeAgent::handleHello(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject&)
{
    sendSuccess(socket, requestId, hello());
    return;
}

void RuntimeAgent::handleHelp(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject&)
{
    sendSuccess(socket, requestId, commandList());
    return;
}

void RuntimeAgent::handleObjectTree(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject& parameters)
{
    QString error;
    const bool hasSelector = parameters.contains(QStringLiteral("objectName"))
        || parameters.contains(QStringLiteral("id"));
    QObject* root = hasSelector ? resolveObject(parameters, error) : m_registry.root();
    if (root == nullptr) {
        sendError(socket, requestId, QStringLiteral("object_not_found"), error);
        return;
    }
    sendSuccess(socket, requestId,
                m_registry.tree(root, parameters.value(QStringLiteral("maxDepth")).toInt(8)));
    return;
}

void RuntimeAgent::handleObjectList(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject&)
{
    sendSuccess(socket, requestId, m_registry.flatList());
    return;
}

void RuntimeAgent::handleObjectDescribe(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject& parameters)
{
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

void RuntimeAgent::handleObjectGet(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject& parameters)
{
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

void RuntimeAgent::handleObjectSet(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject& parameters)
{
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

void RuntimeAgent::handleObjectInvoke(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject& parameters)
{
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

void RuntimeAgent::handleActionTrigger(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject& parameters)
{
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

void RuntimeAgent::handleWidgetClick(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject& parameters)
{
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

void RuntimeAgent::handleEventSubscribe(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject& parameters)
{
    auto iterator = m_clients.find(socket);
    if (iterator == m_clients.end()) {
        sendError(socket, requestId, QStringLiteral("client_gone"), QStringLiteral("client not found"));
        return;
    }
    QStringList prefixes;
    QString error;
    if (!parseEventPrefixes(parameters.value(QStringLiteral("prefixes")), prefixes, error)) {
        sendError(socket, requestId, QStringLiteral("invalid_prefixes"), error);
        return;
    }
    iterator->allEvents = parameters.value(QStringLiteral("all")).toBool(false);
    iterator->eventPrefixes = std::move(prefixes);
    sendSuccess(socket, requestId, QJsonObject{
        {QStringLiteral("all"), iterator->allEvents},
        {QStringLiteral("prefixes"), QJsonArray::fromStringList(iterator->eventPrefixes)},
    });
    return;
}

void RuntimeAgent::handleEventHistory(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject& parameters)
{
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
    QString prefixError;
    if (!parseEventPrefixes(
            parameters.value(QStringLiteral("prefixes")), prefixes, prefixError)) {
        sendError(socket, requestId, QStringLiteral("invalid_prefixes"), prefixError);
        return;
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

void RuntimeAgent::handleModuleList(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject&)
{
    sendSuccess(socket, requestId, m_modules.list());
    return;
}

void RuntimeAgent::handleSnippetLoad(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject& parameters)
{
    QString error;
    RuntimeAgent::LoadedModule* module = m_modules.loadSnippet(
        parameters.value(QStringLiteral("path")).toString(), error);
    if (module == nullptr) {
        sendError(socket, requestId, QStringLiteral("load_failed"), error);
        return;
    }
    QJsonObject result{
        {QStringLiteral("moduleId"), QString::number(module->id)},
        {QStringLiteral("name"), module->name},
        {QStringLiteral("path"), module->path},
        {QStringLiteral("stamped"), module->stamped},
    };
    if (!module->targetBuildId.isEmpty()) {
        result.insert(QStringLiteral("targetBuildId"), module->targetBuildId);
    }
    if (!module->stamped) {
        // A stamped module was compared with this process. Say which side made
        // that comparison impossible so callers do not mistake reported data for
        // verified agreement.
        result.insert(
            QStringLiteral("note"),
            module->targetBuildId.isEmpty()
                ? QStringLiteral("this module reports no host build id, so whether its offsets "
                                 "into application types describe this process is unchecked")
                : QStringLiteral("this process reports no host build id, so the module's "
                                 "reported target build could not be checked"));
    }
    publishEvent(QStringLiteral("snippet.loaded"), result);
    sendSuccess(socket, requestId, result);
    return;
}

void RuntimeAgent::handleSnippetRun(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject& parameters)
{
    quint64 moduleId = 0;
    const bool ok = parseUnsignedInteger(
        parameters.value(QStringLiteral("moduleId")), moduleId);
    RuntimeAgent::LoadedModule* module = m_modules.module(ok ? moduleId : 0);
    if (module == nullptr || module->kind != QLatin1String("snippet")) {
        sendError(socket, requestId, QStringLiteral("module_not_found"),
                  QStringLiteral("snippet module was not found"));
        return;
    }

    const QString executor = parameters.value(QStringLiteral("executor")).toString(
        QStringLiteral("gui"));
    if (!executorExists(executor)) {
        sendError(socket, requestId, QStringLiteral("invalid_executor"),
                  QStringLiteral("executor must be one of: %1").arg(executorChoices()));
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

void RuntimeAgent::handleSnippetRelease(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject& parameters)
{
    quint64 moduleId = 0;
    const bool ok = parseUnsignedInteger(
        parameters.value(QStringLiteral("moduleId")), moduleId);
    RuntimeAgent::LoadedModule* module = m_modules.module(ok ? moduleId : 0);
    if (module == nullptr || module->kind != QLatin1String("snippet")) {
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
    // was used without having to assume.
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
    if (!executorExists(executor)) {
        sendError(socket, requestId, QStringLiteral("invalid_executor"),
                  QStringLiteral("executor must be one of: %1").arg(executorChoices()));
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
            // recorded target can be gone by now: say so instead of quietly
            // running the release somewhere else, because where it
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

void RuntimeAgent::handleStashList(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject&)
{
    sendSuccess(socket, requestId, QJsonObject{
        {QStringLiteral("entries"), stashEntries()},
    });
    return;
}

void RuntimeAgent::handleStashGet(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject& parameters)
{
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
        {QStringLiteral("moduleName"), found->moduleName},
        {QStringLiteral("base64"), QString::fromLatin1(found->bytes.toBase64())},
    };
    lock.unlock();
    sendSuccess(socket, requestId, result);
    return;
}

void RuntimeAgent::handleStashDrop(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject& parameters)
{
    const QString key = parameters.value(QStringLiteral("key")).toString();
    sendSuccess(socket, requestId, QJsonObject{
        {QStringLiteral("key"), key},
        {QStringLiteral("dropped"), stashDrop(key)},
    });
    return;
}

void RuntimeAgent::handleSymbolResolve(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject& parameters)
{
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

void RuntimeAgent::handleUnsafeStatus(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject&)
{
    sendSuccess(socket, requestId, QJsonObject{
        {QStringLiteral("enabled"), m_unsafeEnabled},
        {QStringLiteral("environment"), QStringLiteral("QT_RUNTIME_AGENT_UNSAFE=1")},
    });
    return;
}

void RuntimeAgent::handleUnsafeMemoryRead(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject& parameters)
{
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
        const int errorNumber = errno;
        sendError(socket, requestId, QStringLiteral("memory_read_failed"),
                  failedCall("process_vm_readv", errorNumber));
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

void RuntimeAgent::handleUnsafeMemoryWrite(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject& parameters)
{
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
        const int errorNumber = errno;
        sendError(socket, requestId, QStringLiteral("memory_write_failed"),
                  failedCall("process_vm_writev", errorNumber));
        return;
    }
    sendSuccess(socket, requestId, QJsonObject{
        {QStringLiteral("address"), pointerString(reinterpret_cast<void*>(address))},
        {QStringLiteral("bytesWritten"), static_cast<qint64>(transferred)},
    });
    return;
}

void RuntimeAgent::handleUnsafeCrash(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject&)
{
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

void RuntimeAgent::handleProcessQuit(QLocalSocket* socket,
                                 const QJsonValue& requestId,
                                 const QJsonObject&)
{
    sendSuccess(socket, requestId, true);
    socket->flush();
    QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
    return;
}


void RuntimeAgent::sendSuccess(Client client,
                               const QJsonValue& requestId,
                               const QJsonValue& result)
{
    sendObject(std::move(client), QJsonObject{
        {QStringLiteral("id"), requestId},
        {QStringLiteral("ok"), true},
        {QStringLiteral("result"), result},
    });
}

void RuntimeAgent::sendError(Client client,
                             const QJsonValue& requestId,
                             const QString& code,
                             const QString& message)
{
    sendObject(std::move(client), QJsonObject{
        {QStringLiteral("id"), requestId},
        {QStringLiteral("ok"), false},
        {QStringLiteral("error"), QJsonObject{
            {QStringLiteral("code"), code},
            {QStringLiteral("message"), message},
        }},
    });
}

void RuntimeAgent::sendSuccess(QLocalSocket* socket,
                               const QJsonValue& requestId,
                               const QJsonValue& result)
{
    sendSuccess(Client(this, socket), requestId, result);
}

void RuntimeAgent::sendError(QLocalSocket* socket,
                             const QJsonValue& requestId,
                             const QString& code,
                             const QString& message)
{
    sendError(Client(this, socket), requestId, code, message);
}

void RuntimeAgent::sendObject(Client client, QJsonObject object)
{
    CallbackLease lease(client.m_lifetime);
    if (!lease || lease.get() != this || client.m_owner != this) {
        return;
    }
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this, client = std::move(client), object = std::move(object)]() mutable {
                sendObject(std::move(client), std::move(object));
            },
            Qt::QueuedConnection);
        return;
    }
    auto* socket = qobject_cast<QLocalSocket*>(client.m_socket.data());
    if (socket == nullptr || !m_clients.contains(socket)
        || socket->state() == QLocalSocket::UnconnectedState) {
        return;
    }
    socket->write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    socket->write("\n");
}

void RuntimeAgent::sendObject(QLocalSocket* socket, const QJsonObject& object)
{
    sendObject(Client(this, socket), object);
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

QJsonObject RuntimeAgent::coreHello()
{
#ifdef NDEBUG
    constexpr bool optimizedBuild = true;
#else
    constexpr bool optimizedBuild = false;
#endif
    return QJsonObject{
        {QStringLiteral("name"), QCoreApplication::applicationName()},
        {QStringLiteral("protocolVersion"), 1},
        {QStringLiteral("agentAbi"), QStringLiteral("0x%1").arg(RUNTIME_AGENT_ABI, 8, 16, QLatin1Char('0'))},
        {QStringLiteral("pid"), QCoreApplication::applicationPid()},
        {QStringLiteral("uid"), static_cast<int>(::getuid())},
        {QStringLiteral("socket"), m_socketName},
        {QStringLiteral("qtVersion"), QString::fromLatin1(qVersion())},
        {QStringLiteral("compiler"), QString::fromLatin1(__VERSION__)},
        {QStringLiteral("optimizedBuild"), optimizedBuild},
        {QStringLiteral("unsafeEnabled"), m_unsafeEnabled},
        // The build a module has to have been compiled against for the loader
        // to accept it, readable without loading anything.
        {QStringLiteral("buildId"), runtime_agent::hostBuildId()},
    };
}

QJsonObject RuntimeAgent::hello()
{
    QJsonObject result = coreHello();
    for (auto it = m_helloFields.cbegin(); it != m_helloFields.cend(); ++it) {
        result.insert(it.key(), it.value()());
    }
    return result;
}

QJsonArray RuntimeAgent::commandList() const
{
    QJsonArray result;
    for (const CommandEntry& entry : m_commands) {
        result.append(entry.name);
    }
    return result;
}

RuntimeAgent::ModuleContext* RuntimeAgent::contextForModule(const LoadedModule* module)
{
    if (module == nullptr) {
        return nullptr;
    }
    if (const auto found = m_moduleContexts.find(module->id);
        found != m_moduleContexts.end()) {
        return found->second;
    }

    // A module may retain this pointer in callbacks for the rest of the process.
    // The owning agent can stop accepting callbacks, but the context itself is
    // never freed.
    static auto* const contexts =
        new std::vector<std::unique_ptr<ModuleContext>>();
    static auto* const contextsMutex = new std::mutex();

    auto context = std::make_unique<ModuleContext>();
    context->lifetime = m_callbackLifetime;
    context->module = module;
    ModuleContext* const result = context.get();
    {
        std::lock_guard lock(*contextsMutex);
        contexts->push_back(std::move(context));
    }
    m_moduleContexts.emplace(module->id, result);
    return result;
}

RuntimeAgent::CallbackLease RuntimeAgent::agentOf(void* agentContext)
{
    auto* context = static_cast<ModuleContext*>(agentContext);
    return context != nullptr ? CallbackLease(context->lifetime) : CallbackLease();
}

void RuntimeAgent::runSnippet(RuntimeAgent::LoadedModule* module,
                              const quint64 operationId,
                              const QString& executor,
                              QObject* target,
                              const QJsonValue& request,
                              const SnippetEntry entry)
{
    auto invocation = std::make_shared<SnippetInvocation>();
    invocation->operationId = operationId;
    invocation->entry = entry;
    invocation->executor = executor;
    invocation->target = target;
    invocation->requestJson = compactJsonValue(request);

    invocation->host = RuntimeAgentHost{
        RUNTIME_AGENT_ABI,
        sizeof(RuntimeAgentHost),
        // Not the agent itself: the module's own context, so that a callback
        // made later from a draw hook or a menu handler still identifies the
        // module it came from.
        contextForModule(module),
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
        &RuntimeAgent::hostPatchBind,
        &RuntimeAgent::hostPatchUnbind,
    };

    publishEvent(QStringLiteral("operation.started"), QJsonObject{
        {QStringLiteral("operationId"), QString::number(invocation->operationId)},
        {QStringLiteral("kind"), entry == SnippetEntry::Release
            ? QStringLiteral("snippetRelease") : QStringLiteral("snippet")},
        {QStringLiteral("moduleId"), QString::number(module->id)},
        {QStringLiteral("name"), module->name},
        {QStringLiteral("executor"), executor},
    });

    auto callback = [lifetime = m_callbackLifetime, module, invocation] {
        CallbackLease lease(lifetime);
        if (lease) {
            lease->executeSnippet(module, invocation);
        }
    };

    bool scheduled = true;
    if (const auto registered = m_executors.constFind(executor);
        registered != m_executors.cend()) {
        scheduled = (*registered)(std::move(callback));
    } else if (executor == QStringLiteral("object") && target != nullptr) {
        scheduled = QMetaObject::invokeMethod(target, std::move(callback), Qt::QueuedConnection);
    } else if (QObject* root = m_registry.root(); root != nullptr) {
        scheduled = QMetaObject::invokeMethod(root, std::move(callback), Qt::QueuedConnection);
    } else {
        scheduled = false;
    }

    if (!scheduled) {
        invocation->error = QStringLiteral("Qt rejected the queued snippet invocation");
        invocation->completed = true;
        finishSnippet(module, invocation);
    }
}

void RuntimeAgent::executeSnippet(RuntimeAgent::LoadedModule* module,
                                  const std::shared_ptr<SnippetInvocation>& invocation)
{
    invocation->enteredModule = true;
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

    if (QThread::currentThread() == thread()) {
        finishSnippet(module, invocation);
        return;
    }
    QMetaObject::invokeMethod(
        this,
        [lifetime = m_callbackLifetime, module, invocation] {
            CallbackLease lease(lifetime);
            if (lease) {
                lease->finishSnippet(module, invocation);
            }
        },
        Qt::QueuedConnection);
}

void RuntimeAgent::finishSnippet(
    RuntimeAgent::LoadedModule* module,
    const std::shared_ptr<SnippetInvocation>& invocation)
{
    Q_ASSERT(QThread::currentThread() == thread());

    // All process-lifetime module bookkeeping belongs to the agent thread. The
    // native entry may have run elsewhere, but its observed result arrives here.
    if (invocation->enteredModule && invocation->entry == SnippetEntry::Run) {
        module->lastAttemptedExecutor = invocation->executor;
        module->lastAttemptedTarget = invocation->target;
        module->hadAttemptedRun = true;
    }

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

bool RuntimeAgent::executorExists(const QString& name) const
{
    return name == QStringLiteral("gui") || name == QStringLiteral("object")
        || m_executors.contains(name);
}

QString RuntimeAgent::executorChoices() const
{
    QStringList choices{QStringLiteral("gui"), QStringLiteral("object")};
    choices.append(m_executors.keys());
    return choices.join(QStringLiteral(", "));
}

bool RuntimeAgent::parseEventPrefixes(const QJsonValue& value,
                                      QStringList& prefixes,
                                      QString& error)
{
    prefixes.clear();
    if (value.isUndefined()) {
        return true;
    }
    if (!value.isArray()) {
        error = QStringLiteral("prefixes must be an array of non-empty strings");
        return false;
    }
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isString() || item.toString().isEmpty()) {
            error = QStringLiteral("every event prefix must be a non-empty string");
            return false;
        }
        prefixes.append(item.toString());
    }
    return true;
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
    auto* context = static_cast<ModuleContext*>(agentContext);
    const QString moduleName = context != nullptr && context->module != nullptr
        ? context->module->name : QStringLiteral("unknown");
    const QString prefix = QStringLiteral("[snippet:%1]").arg(moduleName);
    const QString text = QString::fromUtf8(message != nullptr ? message : "");
    switch (level) {
    case RUNTIME_AGENT_LOG_ERROR: qCritical().noquote() << prefix << text; break;
    case RUNTIME_AGENT_LOG_WARNING: qWarning().noquote() << prefix << text; break;
    case RUNTIME_AGENT_LOG_DEBUG: qDebug().noquote() << prefix << text; break;
    default: qInfo().noquote() << prefix << text; break;
    }

    CallbackLease lease = agentOf(agentContext);
    if (!lease) {
        return;
    }
    QJsonObject event{
        {QStringLiteral("level"), level},
        {QStringLiteral("message"), text},
        {QStringLiteral("moduleName"), moduleName},
    };
    if (context->module != nullptr) {
        event.insert(QStringLiteral("moduleId"), QString::number(context->module->id));
    }
    lease->publishEvent(QStringLiteral("snippet.log"), event);
}

void RuntimeAgent::hostEmitEvent(void* agentContext,
                                 const char* name,
                                 const char* objectJson)
{
    CallbackLease lease = agentOf(agentContext);
    if (!lease) {
        return;
    }
    const QString eventName = QString::fromUtf8(name != nullptr ? name : "snippet.event");
    lease->publishEvent(
        eventName,
        parseObjectOrRaw(QByteArray(objectJson != nullptr ? objectJson : "{}")));
}

void* RuntimeAgent::hostFindQObject(void* agentContext, const char* objectName)
{
    CallbackLease lease = agentOf(agentContext);
    return lease
        ? lease->findObject(QString::fromUtf8(objectName != nullptr ? objectName : ""))
        : nullptr;
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
    CallbackLease lease = agentOf(agentContext);
    return lease
        ? static_cast<std::uint64_t>(lease->m_monotonicClock.nsecsElapsed())
        : 0;
}

std::int32_t RuntimeAgent::hostPatchBind(void* agentContext,
                                         void* target,
                                         void* replacement,
                                         const std::uint32_t flags,
                                         RuntimeAgentPatchBinding* out)
{
    auto* context = static_cast<ModuleContext*>(agentContext);
    CallbackLease lease = agentOf(agentContext);
    if (!lease || context == nullptr || context->module == nullptr
        || out == nullptr || target == nullptr || replacement == nullptr
        || !lease->m_patchBind) {
        return RUNTIME_AGENT_PATCH_BIND_REFUSED;
    }

    QString error;
    RuntimeAgentPatchBinding binding{};
    const std::int32_t result = lease->m_patchBind(
        target, replacement, context->module->id, flags, binding, error);
    if (result != RUNTIME_AGENT_PATCH_OK) {
        lease->publishEvent(QStringLiteral("patch.bindRefused"), QJsonObject{
            {QStringLiteral("moduleId"), QString::number(context->module->id)},
            {QStringLiteral("error"), error},
        });
        return result;
    }

    *out = binding;
    return RUNTIME_AGENT_PATCH_OK;
}

std::int32_t RuntimeAgent::hostPatchUnbind(void* agentContext, const std::uint64_t bindingId)
{
    auto* context = static_cast<ModuleContext*>(agentContext);
    CallbackLease lease = agentOf(agentContext);
    if (!lease || context == nullptr || context->module == nullptr
        || !lease->m_patchUnbind) {
        return RUNTIME_AGENT_PATCH_UNBIND_REFUSED;
    }
    QString error;
    return lease->m_patchUnbind(bindingId, context->module->id, error);
}

std::int32_t RuntimeAgent::hostStashPut(void* agentContext,
                                        const char* key,
                                        const void* bytes,
                                        const std::int64_t size,
                                        const std::int32_t overwrite)
{
    auto* context = static_cast<ModuleContext*>(agentContext);
    CallbackLease lease = agentOf(agentContext);
    if (!lease || context == nullptr || context->module == nullptr
        || key == nullptr || *key == '\0' || size < 0
        || (bytes == nullptr && size > 0)) {
        return -2;
    }
    RuntimeAgent* const agent = lease.get();
    const QString name = QString::fromUtf8(key);

    QMutexLocker lock(&agent->m_stashMutex);
    const bool existed = agent->m_stash.contains(name);
    if (existed && overwrite == 0) {
        return -1;
    }
    // Overwrite rights are the one thing identity decides. A record belongs to
    // a snippet, not to one loaded generation of it, so a distinct successor may
    // replace its predecessor's entry while an unrelated module may not. Reads
    // stay open on purpose: a repair module written after the fact has a
    // different name by construction, and shutting it out would remove the
    // reason the namespace is flat.
    if (existed) {
        const QString owner = agent->m_stash.value(name).moduleName;
        if (!owner.isEmpty() && owner != context->module->name) {
            return -2;
        }
    }

    StashEntry entry;
    entry.bytes = QByteArray(static_cast<const char*>(bytes), static_cast<qsizetype>(size));
    entry.monotonicNs = static_cast<quint64>(agent->m_monotonicClock.nsecsElapsed());
    entry.moduleId = context->module->id;
    entry.moduleName = context->module->name;
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
    CallbackLease lease = agentOf(agentContext);
    if (!lease) {
        return -1;
    }
    RuntimeAgent* const agent = lease.get();

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
    auto* context = static_cast<ModuleContext*>(agentContext);
    CallbackLease lease = agentOf(agentContext);
    if (!lease || context == nullptr || context->module == nullptr) {
        return -2;
    }
    if (key == nullptr) {
        return 0;
    }
    RuntimeAgent* const agent = lease.get();
    const QString name = QString::fromUtf8(key);

    // The same rule the overwrite check applies, for the same reason. Without
    // it the rule is trivially avoidable: drop the entry, then put it back as
    // a new one under your own name. It would also let any module drop another
    // one's only good copy, or its claim, which is what protects a region from
    // being taken while it is still displaced.
    //
    // The protocol-level stash.drop stays unrestricted. Deciding that a
    // restore worked, or that a dead module's claim should be released, is the
    // driver's judgement, and it is not a module.
    QMutexLocker lock(&agent->m_stashMutex);
    const auto found = agent->m_stash.find(name);
    if (found == agent->m_stash.end()) {
        return 0;
    }
    if (!found->moduleName.isEmpty() && found->moduleName != context->module->name) {
        return -2;
    }
    agent->m_stash.erase(found);
    return 1;
}

std::int64_t RuntimeAgent::hostStashList(void* agentContext,
                                         char* buffer,
                                         const std::int64_t capacity)
{
    if (capacity < 0 || (buffer == nullptr && capacity > 0)) {
        return -1;
    }
    CallbackLease lease = agentOf(agentContext);
    if (!lease) {
        return -1;
    }
    const QByteArray json =
        QJsonDocument(lease->stashEntries()).toJson(QJsonDocument::Compact);
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
            {QStringLiteral("moduleName"), it->moduleName},
        });
    }
    return entries;
}

bool RuntimeAgent::stashDrop(const QString& key)
{
    QMutexLocker lock(&m_stashMutex);
    return m_stash.remove(key) > 0;
}
