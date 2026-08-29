#pragma once

#include "agent/agent_abi.h"
#include "agent/module_registry.h"
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

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

class QLocalSocket;

class RuntimeAgent final : public QObject {
    Q_OBJECT

    struct CallbackLifetime;
    class CallbackLease;

public:
    // A guarded client reference for command handlers. It may be copied into a
    // delayed callback: replies check both the guarded socket and the agent's
    // current client table instead of dereferencing a retained raw pointer.
    class Client final {
    public:
        Client() = default;

    private:
        friend class RuntimeAgent;
        Client(RuntimeAgent* owner, QLocalSocket* socket);

        RuntimeAgent* m_owner = nullptr;
        std::shared_ptr<CallbackLifetime> m_lifetime;
        QPointer<QObject> m_socket;
    };

    explicit RuntimeAgent(QObject* root,
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

    using CommandHandler = std::function<void(Client client,
                                              const QJsonValue& requestId,
                                              const QJsonObject& parameters)>;

    // Adds one command. parameters lists every key the handler reads, and a
    // request naming anything else is refused before the handler runs, so the
    // list is what the refusal quotes back. The handler answers with
    // sendSuccess or sendError, once, and may do it later than it is called. A
    // copied Client stays safe after disconnect: the reply is simply dropped.
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
    // places already, the thread owning the object a request names and the root
    // object's thread, and an application adds any other place it can run one,
    // such as the thread holding its render context.
    //
    // Returns false and registers nothing if the name is taken.
    [[nodiscard]] bool registerExecutor(QString name, SnippetExecutor executor);

    // What this application adds to hello. Read on every hello, so it reports
    // the state at that moment. Returns false if the core or another
    // registration already writes that field.
    [[nodiscard]] bool registerHelloField(QString name, std::function<QJsonValue()> value);

    // The host ABI always offers patch_bind and patch_unbind, while the policy
    // and target set belong to the application. Without a provider the callbacks
    // refuse. Only one provider can be registered.
    using PatchBindHandler = std::function<std::int32_t(
        void* target,
        void* replacement,
        quint64 owner,
        std::uint32_t flags,
        RuntimeAgentPatchBinding& binding,
        QString& error)>;
    using PatchUnbindHandler = std::function<std::int32_t(
        std::uint64_t bindingId,
        quint64 owner,
        QString& error)>;

    [[nodiscard]] bool registerPatchProvider(PatchBindHandler bind,
                                             PatchUnbindHandler unbind);

    // Protocol integers accept either an exact JSON number or its decimal string
    // form. Application handlers use the same parser as the core.
    [[nodiscard]] static bool parseUnsignedInteger(const QJsonValue& value,
                                                   quint64& result);

    void sendSuccess(Client client,
                     const QJsonValue& requestId,
                     const QJsonValue& result = {});
    void sendError(Client client,
                   const QJsonValue& requestId,
                   const QString& code,
                   const QString& message);

private:
    struct ClientState {
        QByteArray input;
        bool allEvents = true;
        QStringList eventPrefixes;
    };

    using LoadedModule = runtime_agent::ModuleRegistry::LoadedModule;

    // Which of a module's two entry points an invocation calls. Both take the
    // same host and report the same way, so they share all the machinery below;
    // only the pointer called and the event kind differ.
    enum class SnippetEntry { Run, Release };

    struct SnippetInvocation {
        quint64 operationId = 0;
        QByteArray requestJson;
        QByteArray resultJson;
        QString error;
        bool completed = false;
        bool enteredModule = false;
        SnippetEntry entry = SnippetEntry::Run;
        QString executor;
        QPointer<QObject> target;
        RuntimeAgentHost host{};
    };

    // What a module's host callbacks carry as agent_context, instead of the
    // agent itself. A callback made from outside an invocation, such as a draw
    // hook or a menu handler, has no other way to say which module it came from,
    // so stash entries and log lines from those places would otherwise be
    // unattributable. The registry and its modules live for the process and
    // modules are never unloaded, so retaining the registry entry avoids a
    // second copy of its identity and keeps this pointer valid.
    struct ModuleContext {
        std::shared_ptr<CallbackLifetime> lifetime;
        const LoadedModule* module = nullptr;
    };

    struct StashEntry {
        QByteArray bytes;
        quint64 monotonicNs = 0;

        // Both stamped by the host, never supplied by the depositor, which is
        // what makes them worth checking. The id is exact provenance: which
        // load wrote this. The name is identity across distinct generations of
        // the same source, which have different loader instances and ids. A rule
        // keyed on the id would refuse the successor its predecessor's record,
        // which is the handover the stash exists to support.
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
    void sendObject(Client client, QJsonObject object);
    void sendObject(QLocalSocket* socket, const QJsonObject& object);

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

    void runSnippet(LoadedModule* module,
                    quint64 operationId,
                    const QString& executor,
                    QObject* target,
                    const QJsonValue& request,
                    SnippetEntry entry = SnippetEntry::Run);
    void executeSnippet(LoadedModule* module,
                        const std::shared_ptr<SnippetInvocation>& invocation);
    void finishSnippet(LoadedModule* module,
                       const std::shared_ptr<SnippetInvocation>& invocation);

    [[nodiscard]] QJsonValue parseSnippetResult(const QByteArray& json) const;
    [[nodiscard]] bool executorExists(const QString& name) const;
    [[nodiscard]] QString executorChoices() const;
    [[nodiscard]] static bool parseEventPrefixes(const QJsonValue& value,
                                                 QStringList& prefixes,
                                                 QString& error);
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

    [[nodiscard]] ModuleContext* contextForModule(const LoadedModule* module);
    [[nodiscard]] static CallbackLease agentOf(void* agentContext);
    [[nodiscard]] QJsonArray stashEntries() const;
    [[nodiscard]] bool stashDrop(const QString& key);

    // Process-lifetime storage for every native module. Descriptor-specific
    // loaders remain in the application layer and adopt their records here.
    runtime_agent::ModuleRegistry& m_modules;

    // Native callbacks and executor work acquire this before using agent-owned
    // state. Destruction closes it and waits for calls already inside, while work
    // that starts later receives a defined refusal.
    std::shared_ptr<CallbackLifetime> m_callbackLifetime;

    QString m_socketName;
    QLocalServer m_server;
    QHash<QLocalSocket*, ClientState> m_clients;
    std::vector<CommandEntry> m_commands;
    QMap<QString, SnippetExecutor> m_executors;
    QMap<QString, std::function<QJsonValue()>> m_helloFields;
    PatchBindHandler m_patchBind;
    PatchUnbindHandler m_patchUnbind;
    ObjectRegistry m_registry;
    QElapsedTimer m_monotonicClock;
    quint64 m_eventSequence = 0;
    QQueue<QJsonObject> m_eventHistory;
    // Keep native snippet operation IDs in the high half, leaving applications
    // the low half for their own operation events without coordination.
    quint64 m_nextOperationId = (quint64{1} << 63);
    bool m_unsafeEnabled = false;

    // The pointed-to contexts are allocated once and retained for the process.
    // This map is only this agent's lookup table and does not own them.
    std::unordered_map<quint64, ModuleContext*> m_moduleContexts;

    // Reachable from any thread, since a module may stash from a worker-thread
    // callback as easily as from the GUI thread.
    mutable QMutex m_stashMutex;
    QMap<QString, StashEntry> m_stash;
};
