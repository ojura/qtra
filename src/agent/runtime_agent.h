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

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

class MainWindow;
class QLocalSocket;

class RuntimeAgent final : public QObject {
    Q_OBJECT

public:
    explicit RuntimeAgent(MainWindow* window,
                          ModuleManager& modules,
                          QString socketName,
                          QObject* parent = nullptr);
    ~RuntimeAgent() override;

    [[nodiscard]] bool start(QString& error);
    [[nodiscard]] QString socketName() const { return m_socketName; }
    [[nodiscard]] bool unsafeEnabled() const noexcept { return m_unsafeEnabled; }

    [[nodiscard]] QObject* findObject(const QString& objectName) const;
    void publishEvent(const QString& name, const QJsonObject& data = {});

    // What an application adds to the protocol.
    //
    // The table, dispatch, the two refusals, event sequence numbers, history
    // and subscription are all here. What is being controlled is not, so the
    // application writes the commands that reach it and connects its own
    // signals to publishEvent.

    using CommandHandler = std::function<void(QLocalSocket* socket,
                                              const QJsonValue& requestId,
                                              const QJsonObject& parameters)>;

    // Adds one command. parameters lists every key the handler reads, and a
    // request naming anything else is refused before the handler runs, so the
    // list is what the refusal quotes back. The handler answers with
    // sendSuccess or sendError, once, and may do it later than it is called.
    //
    // Returns false and registers nothing if the name is taken, because a
    // second entry under one name is unreachable and help would list the name
    // twice.
    [[nodiscard]] bool registerCommand(QString name,
                                       QStringList parameters,
                                       CommandHandler handler);

    // Somewhere a snippet can run. Answers false when it could not schedule
    // the call, and the run is then reported as having failed.
    using SnippetExecutor = std::function<bool(std::function<void()> call)>;

    // Adds one under the name a request asks for it by. The core knows two
    // places already, the thread owning the object a request names and the
    // window's, and an application adds any other place it can run one, such
    // as the thread holding its render context.
    //
    // Returns false and registers nothing if the name is taken.
    [[nodiscard]] bool registerExecutor(QString name, SnippetExecutor executor);

    // What this application adds to hello. Read on every hello, so it reports
    // the state at that moment. Returns false if the core or another
    // registration already writes that field.
    [[nodiscard]] bool registerHelloField(QString name, std::function<QJsonValue()> value);

    void sendSuccess(QLocalSocket* socket,
                     const QJsonValue& requestId,
                     const QJsonValue& result = {});
    void sendError(QLocalSocket* socket,
                   const QJsonValue& requestId,
                   const QString& code,
                   const QString& message);

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

    static void sendObject(QLocalSocket* socket, const QJsonObject& object);

    // One entry per command. dispatchRequest looks the name up in m_commands
    // and calls through; nothing else selects on a command name.
    struct CommandEntry {
        QString name;

        // Every parameter this command reads. A request naming anything else is
        // refused, because a parameter the host drops is a request that
        // succeeded and did nothing, and the caller has no way to tell.
        //
        // That is not hypothetical: both tools kept sending a mode parameter
        // for months after the thing it selected was removed, and every call
        // succeeded, so nothing ever said the option had stopped meaning
        // anything. Object selectors are why the empty list means no
        // parameters at all and not "anything goes".
        QStringList parameters;

        CommandHandler handler;
    };

    // Every command the core answers itself, and the only place that set is
    // written down. Called from the constructor, before anything can dispatch.
    void registerCoreCommands();

    void handleHello(QLocalSocket* socket,
                    const QJsonValue& requestId,
                    const QJsonObject& parameters);
    void handleHelp(QLocalSocket* socket,
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
    // What the core knows about itself, without what applications have
    // registered. Split out so registerHelloField can refuse a field the core
    // already writes instead of silently losing to it.
    [[nodiscard]] QJsonObject coreHello();
    [[nodiscard]] QJsonObject hello();
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
                                      std::uint32_t flags,
                                      RuntimeAgentPatchBinding* out);
    static std::int32_t hostPatchUnbind(void* agentContext, std::uint64_t bindingId);
    static std::int64_t hostStashList(void* agentContext, char* buffer, std::int64_t capacity);

    [[nodiscard]] ModuleContext* contextForModule(quint64 moduleId);
    [[nodiscard]] static RuntimeAgent* agentOf(void* agentContext);
    [[nodiscard]] QJsonArray stashEntries() const;
    [[nodiscard]] bool stashDrop(const QString& key);

    QPointer<MainWindow> m_window;

    // Owned by whoever built the application, because rolling the active patch
    // back on the way out makes the patched thing emit, and an application's
    // event connections are still live while this object's members go away.
    // Outliving the agent is what keeps that emission from reaching them.
    ModuleManager& m_modules;

    QString m_socketName;
    QLocalServer m_server;
    QHash<QLocalSocket*, ClientState> m_clients;
    std::vector<CommandEntry> m_commands;
    QMap<QString, SnippetExecutor> m_executors;
    QMap<QString, std::function<QJsonValue()>> m_helloFields;
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
};
