#pragma once

// Loaded native modules: what they are, who they belong to, and where they ran.
//
// Everything here holds for any application that loads code at runtime. What a
// module's descriptor means is the caller's business: this opens the object,
// refuses one built against a different host, gives it an identity nothing
// reuses, and remembers where it ran so a release can be sent to the same
// place. It knows nothing about cubes, entries or gateways.
//
// Separate from what owns a patched entry. That is keyed by the function being
// patched and lives for the whole process, because the text of the process
// refers to it. A module's lifetime answers a different question, which is who
// may still call into it, and putting the two in one object would tie each to
// the other's rules.

#include "agent/agent_abi.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QString>

#include <memory>
#include <unordered_map>

namespace runtime_agent {

class ModuleRegistry final {
public:
    struct LoadedModule {
        quint64 id = 0;
        QString path;
        QString name;

        // What the loader that adopted this one calls it. Free text, because
        // the registry does not interpret descriptors and a fixed set here
        // would have to grow for every application that adds a kind.
        QString kind;

        void* handle = nullptr;

        // The generic host ABI's descriptor, when the module carries one.
        const RuntimeAgentSnippet* snippet = nullptr;

        // Whatever else the adopting loader resolved, for it to interpret.
        const void* descriptor = nullptr;

        // How this module was last run successfully, which is what its release
        // runs under unless the caller says otherwise. This is observed and not
        // declared, so it cannot go stale the way a descriptor field would when
        // someone changes the install path and forgets to update it.
        //
        // The last success and not the first attempt: a snippet needing a GL
        // context fails under gui with "use executor=render" and is retried,
        // and a release under the executor that already failed fails the same
        // way. The target is held weakly because it can be gone by release
        // time, whether a worker that finished or an object already destroyed,
        // and a caller is better told that than silently given a different one.
        QString lastExecutor;
        QPointer<QObject> lastTarget;
        bool hadSuccessfulRun = false;

        // Where the module's code actually ran, recorded for every attempt
        // whether or not it completed. A run that installs something and then
        // fails leaves that state behind with no successful record, and it is
        // the only witness to where the install happened.
        //
        // Releasing such a module under a guessed executor is worse than not
        // releasing it at all. Qt is specific about what may not cross threads:
        // removeEventFilter must run on the watched object's thread, a timer
        // must be killed from its own, and deleting a QObject from another
        // thread is forbidden. A release that faithfully undoes a worker-thread
        // install from the GUI thread breaks those rules, and nothing checks
        // thread affinity the way the scene snippets check the GL context, so
        // it fails as a race inside the process and not as an error anyone can
        // see. An attempt, by definition, is somewhere the install code already
        // ran.
        //
        // Both records are heuristics under a mixed history, in the same way
        // and with the same consequence. An old success under one executor
        // followed by a newer failed install under another resolves to the old
        // one; an install under one executor followed by a refused attempt
        // under another resolves to the refused one. Either way a release that
        // checks its context refuses loudly and the handover aborts, which is
        // the failure worth having.
        QString lastAttemptedExecutor;
        QPointer<QObject> lastAttemptedTarget;
        bool hadAttemptedRun = false;

        // The host build this module reports having been compiled against, and
        // whether that value was actually compared with this process. A module
        // reporting a different non-empty build never gets this far. Stamped is
        // false when either side reports no build id, because no agreement was
        // established in that case.
        QString targetBuildId;
        bool stamped = false;

        // Whether the module carries a release entry point. The loader refuses
        // any descriptor whose layout does not match this host exactly, so the
        // field is always present and only its value is in question.
        [[nodiscard]] bool declaresRelease() const
        {
            return snippet != nullptr && snippet->release != nullptr;
        }
    };

    // The one every adapter shares, allocated once and never destroyed.
    //
    // For the same reason the patched entries have one. A generation names the
    // module that bound it, and generations outlive whoever installed them, so
    // a registry that died with its adapter would leave a selected binding
    // whose owner nothing could name and nothing could release. The code those
    // modules hold is never unloaded either, so there is nothing here to
    // reclaim even in principle.
    [[nodiscard]] static ModuleRegistry& instance();

    ModuleRegistry() = default;
    ModuleRegistry(const ModuleRegistry&) = delete;
    ModuleRegistry& operator=(const ModuleRegistry&) = delete;

    // Deliberately does not dlclose. A module may have installed callbacks
    // whose machine code and static storage live in it, and a patched entry may
    // still reach code it owns.
    ~ModuleRegistry() = default;

    // Loads a module carrying the generic host ABI's descriptor.
    [[nodiscard]] LoadedModule* loadSnippet(const QString& path, QString& error);

    // Opens a module and checks it was built against the running host, leaving
    // what its descriptor means to the caller. buildId is what the module
    // reported; stamped is true only when that value was compared with a
    // non-empty host id and matched.
    //
    // On success the handle belongs to the caller until it either adopts a
    // module holding it or gives it back through closeUnadopted. A caller that
    // resolves no usable descriptor must do the latter, because a handle
    // nothing refers to is the one case where closing is right.
    [[nodiscard]] void* open(const QString& path,
                             QString& buildId,
                             bool& stamped,
                             QString& error);
    static void closeUnadopted(void* handle);

    // Takes ownership of a module the caller has finished describing. If the
    // dynamic loader returned a handle already registered here, balances the
    // extra loader reference and returns that existing record instead of minting
    // a second identity for one module instance.
    [[nodiscard]] LoadedModule* adopt(std::unique_ptr<LoadedModule> module);

    // An id nothing reuses for the life of the process. A binding outlives the
    // registry that made its module: what owns a patched entry keeps
    // generations whatever happens to whoever installed them, and a generation
    // names its owner. Numbering from one again would hand a new module the
    // identity of one whose binding is still selected.
    [[nodiscard]] static quint64 nextId();

    [[nodiscard]] LoadedModule* module(quint64 id) const;
    [[nodiscard]] LoadedModule* moduleForHandle(void* handle) const;
    [[nodiscard]] QJsonArray list() const;

    [[nodiscard]] static QJsonObject describe(const LoadedModule& module);

private:
    std::unordered_map<quint64, std::unique_ptr<LoadedModule>> m_modules;
    std::unordered_map<void*, LoadedModule*> m_modulesByHandle;
};

} // namespace runtime_agent
