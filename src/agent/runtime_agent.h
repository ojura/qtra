#pragma once

#include "agent/agent_abi.h"
#include "agent/module_manager.h"
#include "agent/object_registry.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QLocalServer>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QStringList>

#include <memory>
#include <unordered_map>
#include <vector>

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

    // Which of a module's two entry points an invocation calls. Both take the
    // same host and report the same way, so they share all the machinery below;
    // only the pointer called and the event kind differ.
    enum class SnippetEntry { Run, Release };

    struct SnippetInvocation {
        RuntimeAgent* agent = nullptr;
        quint64 operationId = 0;
        QByteArray requestJson;
        QByteArray resultJson;
        QString error;
        bool completed = false;
        SnippetEntry entry = SnippetEntry::Run;
        QString executor;
        QPointer<QObject> target;
        RuntimeAgentHost host{};
    };

    // What a module's host callbacks carry as agent_context, instead of the
    // agent itself. A callback made from outside an invocation, such as a draw
    // hook or a menu handler, has no other way to say which module it came from, so
    // stash entries and log lines from those places would otherwise be
    // unattributable. Modules are never unloaded, so these satisfy the ABI's
    // promise that agent_context stays valid for the life of the process.
    struct ModuleContext {
        RuntimeAgent* agent = nullptr;
        quint64 moduleId = 0;
        QString moduleName;
    };

    struct StashEntry {
        QByteArray bytes;
        quint64 monotonicNs = 0;

        // Both stamped by the host, never supplied by the depositor, which is
        // what makes them worth checking. The id is exact provenance: which
        // load wrote this. The name is identity across generations, because a
        // reload gives the same source a new id, and a rule keyed on the id
        // would refuse the successor its predecessor's record, which is the
        // handover the stash exists to support.
        quint64 moduleId = 0;
        QString moduleName;
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

    // One command handler each, named for the command. dispatchRequest looks
    // the name up in commands() and calls through; nothing else selects on it.
    struct CommandEntry {
        const char* name;
        void (RuntimeAgent::*handler)(QLocalSocket*, const QJsonValue&, const QJsonObject&);
    };
    [[nodiscard]] static const std::vector<CommandEntry>& commands();

    void handleHello(QLocalSocket* socket,
                    const QJsonValue& requestId,
                    const QJsonObject& parameters);
    void handleHelp(QLocalSocket* socket,
                   const QJsonValue& requestId,
                   const QJsonObject& parameters);
    void handleCubeState(QLocalSocket* socket,
                        const QJsonValue& requestId,
                        const QJsonObject& parameters);
    void handleCubePause(QLocalSocket* socket,
                        const QJsonValue& requestId,
                        const QJsonObject& parameters);
    void handleCubeResume(QLocalSocket* socket,
                         const QJsonValue& requestId,
                         const QJsonObject& parameters);
    void handleCubeReset(QLocalSocket* socket,
                        const QJsonValue& requestId,
                        const QJsonObject& parameters);
    void handleCubeSpeed(QLocalSocket* socket,
                        const QJsonValue& requestId,
                        const QJsonObject& parameters);
    void handleCubeWireframe(QLocalSocket* socket,
                            const QJsonValue& requestId,
                            const QJsonObject& parameters);
    void handleCubeCapture(QLocalSocket* socket,
                          const QJsonValue& requestId,
                          const QJsonObject& parameters);
    void handleObjectTree(QLocalSocket* socket,
                         const QJsonValue& requestId,
                         const QJsonObject& parameters);
    void handleObjectList(QLocalSocket* socket,
                         const QJsonValue& requestId,
                         const QJsonObject& parameters);
    void handleObjectDescribe(QLocalSocket* socket,
                             const QJsonValue& requestId,
                             const QJsonObject& parameters);
    void handleObjectGet(QLocalSocket* socket,
                        const QJsonValue& requestId,
                        const QJsonObject& parameters);
    void handleObjectSet(QLocalSocket* socket,
                        const QJsonValue& requestId,
                        const QJsonObject& parameters);
    void handleObjectInvoke(QLocalSocket* socket,
                           const QJsonValue& requestId,
                           const QJsonObject& parameters);
    void handleActionTrigger(QLocalSocket* socket,
                            const QJsonValue& requestId,
                            const QJsonObject& parameters);
    void handleWidgetClick(QLocalSocket* socket,
                          const QJsonValue& requestId,
                          const QJsonObject& parameters);
    void handleEventSubscribe(QLocalSocket* socket,
                             const QJsonValue& requestId,
                             const QJsonObject& parameters);
    void handleEventHistory(QLocalSocket* socket,
                           const QJsonValue& requestId,
                           const QJsonObject& parameters);
    void handleModuleList(QLocalSocket* socket,
                         const QJsonValue& requestId,
                         const QJsonObject& parameters);
    void handleSnippetLoad(QLocalSocket* socket,
                          const QJsonValue& requestId,
                          const QJsonObject& parameters);
    void handleSnippetRun(QLocalSocket* socket,
                         const QJsonValue& requestId,
                         const QJsonObject& parameters);
    void handleSnippetRelease(QLocalSocket* socket,
                             const QJsonValue& requestId,
                             const QJsonObject& parameters);
    void handleStashList(QLocalSocket* socket,
                        const QJsonValue& requestId,
                        const QJsonObject& parameters);
    void handleStashGet(QLocalSocket* socket,
                       const QJsonValue& requestId,
                       const QJsonObject& parameters);
    void handleStashDrop(QLocalSocket* socket,
                        const QJsonValue& requestId,
                        const QJsonObject& parameters);
    void handlePatchLoad(QLocalSocket* socket,
                        const QJsonValue& requestId,
                        const QJsonObject& parameters);
    void handlePatchActivate(QLocalSocket* socket,
                            const QJsonValue& requestId,
                            const QJsonObject& parameters);
    void handlePatchRollback(QLocalSocket* socket,
                            const QJsonValue& requestId,
                            const QJsonObject& parameters);
    void handlePatchStatus(QLocalSocket* socket,
                          const QJsonValue& requestId,
                          const QJsonObject& parameters);
    void handleSymbolResolve(QLocalSocket* socket,
                            const QJsonValue& requestId,
                            const QJsonObject& parameters);
    void handleUnsafeStatus(QLocalSocket* socket,
                           const QJsonValue& requestId,
                           const QJsonObject& parameters);
    void handleUnsafeMemoryRead(QLocalSocket* socket,
                               const QJsonValue& requestId,
                               const QJsonObject& parameters);
    void handleUnsafeMemoryWrite(QLocalSocket* socket,
                                const QJsonValue& requestId,
                                const QJsonObject& parameters);
    void handleUnsafeCrash(QLocalSocket* socket,
                          const QJsonValue& requestId,
                          const QJsonObject& parameters);
    void handleProcessQuit(QLocalSocket* socket,
                          const QJsonValue& requestId,
                          const QJsonObject& parameters);

    [[nodiscard]] QObject* resolveObject(const QJsonObject& parameters,
                                         QString& error) const;
    [[nodiscard]] QJsonObject cubeState() const;
    [[nodiscard]] QJsonObject hello() const;
    [[nodiscard]] QJsonArray commandList() const;

    void runSnippet(ModuleManager::LoadedModule* module,
                    quint64 operationId,
                    const QString& executor,
                    QObject* target,
                    const QJsonValue& request,
                    SnippetEntry entry = SnippetEntry::Run);
    void executeSnippet(ModuleManager::LoadedModule* module,
                        const std::shared_ptr<SnippetInvocation>& invocation);
    void finishSnippet(ModuleManager::LoadedModule* module,
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
    static std::int32_t hostStashPut(void* agentContext,
                                     const char* key,
                                     const void* bytes,
                                     std::int64_t size,
                                     std::int32_t overwrite);
    static std::int64_t hostStashGet(void* agentContext,
                                     const char* key,
                                     void* buffer,
                                     std::int64_t capacity);
    static std::int32_t hostStashDrop(void* agentContext, const char* key);
    static std::int32_t hostPatchBind(void* agentContext,
                                      void* target,
                                      void* replacement,
                                      RuntimeAgentPatchBinding* out);
    static std::int32_t hostPatchUnbind(void* agentContext, std::uint64_t bindingId);
    static std::int64_t hostStashList(void* agentContext, char* buffer, std::int64_t capacity);

    [[nodiscard]] ModuleContext* contextForModule(quint64 moduleId);
    [[nodiscard]] static RuntimeAgent* agentOf(void* agentContext);
    [[nodiscard]] QJsonArray stashEntries() const;
    [[nodiscard]] bool stashDrop(const QString& key);

    QPointer<MainWindow> m_window;
    QPointer<CubeWidget> m_cube;
    QString m_socketName;
    QLocalServer m_server;
    QHash<QLocalSocket*, ClientState> m_clients;
    ObjectRegistry m_registry;
    QElapsedTimer m_monotonicClock;
    quint64 m_eventSequence = 0;
    QQueue<QJsonObject> m_eventHistory;
    // Keep native snippet operation IDs disjoint from MainWindow's small demo
    // job IDs while retaining a plain uint64 wire representation.
    quint64 m_nextOperationId = (quint64{1} << 63);
    bool m_unsafeEnabled = false;

    std::unordered_map<quint64, std::unique_ptr<ModuleContext>> m_moduleContexts;

    // Reachable from any thread, since a module may stash from a worker-thread
    // callback as easily as from the GUI thread.
    mutable QMutex m_stashMutex;
    QMap<QString, StashEntry> m_stash;

    // Declared last so it is destroyed first. ~ModuleManager rolls the active
    // patch back, which makes CubeWidget emit stateChanged, and the handler for
    // that signal reads m_eventHistory and m_monotonicClock above.
    ModuleManager m_modules;
};
