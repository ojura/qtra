#pragma once

#include "agent/agent_abi.h"
#include "agent/module_manager.h"
#include "agent/object_registry.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QLocalServer>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <memory>

class CubeWidget;
class MainWindow;
class QLocalSocket;

class RuntimeAgent final : public QObject {
    Q_OBJECT

public:
    explicit RuntimeAgent(MainWindow* window,
                          CubeWidget* cube,
                          QString socketName,
                          QObject* parent = nullptr);
    ~RuntimeAgent() override;

    [[nodiscard]] bool start(QString& error);
    [[nodiscard]] QString socketName() const { return m_socketName; }
    [[nodiscard]] bool unsafeEnabled() const noexcept { return m_unsafeEnabled; }

    [[nodiscard]] QObject* findObject(const QString& objectName) const;
    void publishEvent(const QString& name, const QJsonObject& data = {});

private:
    struct ClientState {
        QByteArray input;
        bool allEvents = true;
        QStringList eventPrefixes;
    };

    struct SnippetInvocation {
        RuntimeAgent* agent = nullptr;
        quint64 operationId = 0;
        QByteArray requestJson;
        QByteArray resultJson;
        QString error;
        bool completed = false;
        RuntimeAgentHostV1 host{};
    };

    void acceptConnections();
    void readClient(QLocalSocket* socket);
    void removeClient(QLocalSocket* socket);
    void handleLine(QLocalSocket* socket, const QByteArray& line);
    void dispatchRequest(QLocalSocket* socket,
                         const QJsonValue& requestId,
                         const QString& command,
                         const QJsonObject& parameters);

    void sendSuccess(QLocalSocket* socket,
                     const QJsonValue& requestId,
                     const QJsonValue& result = {});
    void sendError(QLocalSocket* socket,
                   const QJsonValue& requestId,
                   const QString& code,
                   const QString& message);
    static void sendObject(QLocalSocket* socket, const QJsonObject& object);

    [[nodiscard]] QObject* resolveObject(const QJsonObject& parameters,
                                         QString& error) const;
    [[nodiscard]] QJsonObject cubeState() const;
    [[nodiscard]] QJsonObject hello() const;
    [[nodiscard]] QJsonArray commandList() const;

    void runSnippet(ModuleManager::LoadedModule* module,
                    const QString& executor,
                    QObject* target,
                    const QJsonValue& request);
    void executeSnippet(ModuleManager::LoadedModule* module,
                        const std::shared_ptr<SnippetInvocation>& invocation);

    [[nodiscard]] QJsonValue parseSnippetResult(const QByteArray& json) const;
    [[nodiscard]] bool clientWantsEvent(const ClientState& state,
                                        const QString& eventName) const;

    // Stable host ABI callbacks.
    static void hostLog(void* agentContext, std::int32_t level, const char* message);
    static void hostEmitEvent(void* agentContext, const char* name, const char* objectJson);
    static void* hostFindQObject(void* agentContext, const char* objectName);
    static void* hostFindSymbol(void* agentContext, const char* symbolName);
    static const char* hostRequestJson(void* invocationContext);
    static void hostCompleteJson(void* invocationContext, const char* resultJson);
    static void hostFail(void* invocationContext, const char* error);
    static std::uint64_t hostMonotonicTimeNs(void* agentContext);

    QPointer<MainWindow> m_window;
    QPointer<CubeWidget> m_cube;
    QString m_socketName;
    QLocalServer m_server;
    QHash<QLocalSocket*, ClientState> m_clients;
    ObjectRegistry m_registry;
    ModuleManager m_modules;
    QElapsedTimer m_monotonicClock;
    quint64 m_eventSequence = 0;
    quint64 m_nextOperationId = 1;
    bool m_unsafeEnabled = false;
};
