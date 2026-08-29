#pragma once

#include "agent/agent_abi.h"
#include "agent/coverage_manifest.h"
#include "agent/patch_manager.h"
#include "demo/cube_step_abi.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

class CubeWidget;

class ModuleManager final {
public:
    enum class Kind { Snippet, CubePatch };

    struct LoadedModule {
        quint64 id = 0;
        QString path;
        QString name;
        Kind kind = Kind::Snippet;
        void* handle = nullptr;
        const RuntimeAgentSnippet* snippet = nullptr;
        const CubeStepPatch* cubePatch = nullptr;

        // How this module was last run successfully, which is what its release
        // runs under unless the caller says otherwise. This is observed rather
        // than declared, so it cannot go stale the way a descriptor field would
        // when someone changes the install path and forgets to update it.
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
        // it fails as a race inside the process and not as an error anyone
        // can see. An attempt, by definition, is somewhere the install code
        // already ran.
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
        // whether it reported one at all. A module that reports a different
        // build never gets this far, so a stamped module here agrees with the
        // running executable. An unstamped one was built without the define the
        // build supplies, and nothing about its offsets into application types
        // has been checked.
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

    explicit ModuleManager(CubeWidget* cube);
    ~ModuleManager();

    ModuleManager(const ModuleManager&) = delete;
    ModuleManager& operator=(const ModuleManager&) = delete;

    [[nodiscard]] LoadedModule* loadSnippet(const QString& path, QString& error);
    [[nodiscard]] LoadedModule* loadCubePatch(const QString& path, QString& error);
    [[nodiscard]] LoadedModule* module(quint64 id) const;
    [[nodiscard]] QJsonArray list() const;

    // acceptIncompleteCoverage proceeds when the build could not establish that
    // replacing the target reaches every call. It is a request a caller has to
    // make, because the default is to refuse what nobody has shown to be safe.
    [[nodiscard]] bool activateEntryPatch(quint64 id,
                                          bool acceptIncompleteCoverage,
                                          QString& error);
    [[nodiscard]] QJsonObject coverage() const;

private:
    // The one place that decides whether a write may go ahead, so both the
    // protocol and the host binding call answer to the same rules.
    [[nodiscard]] bool admits(bool acceptIncompleteCoverage, QString& error);

public:
    [[nodiscard]] bool rollback(QString& error);

    // Binding a replacement on behalf of a loaded module. Returns 0, or the
    // negative code the host ABI documents.
    [[nodiscard]] int bindReplacement(void* target,
                                      void* replacement,
                                      quint64 owner,
                                      bool acceptIncompleteCoverage,
                                      runtime_agent::PatchBinding& binding,
                                      QString& error);
    [[nodiscard]] int releaseBinding(std::uint64_t bindingId, quint64 owner, QString& error);
    [[nodiscard]] QJsonObject patchStatus() const;

private:
    [[nodiscard]] LoadedModule* insertModule(std::unique_ptr<LoadedModule> module);
    [[nodiscard]] bool resetActivePatch(QString& error);
    [[nodiscard]] static QJsonObject moduleJson(const LoadedModule& module);

    CubeWidget* m_cube = nullptr;
    quint64 m_nextId = 1;
    std::unordered_map<quint64, std::unique_ptr<LoadedModule>> m_modules;
    runtime_agent::PatchManager m_patches;
    quint64 m_activeEntryModule = 0;
    // The protocol's own binding, so releasing it goes through the same
    // generation rule a snippet's binding does.
    std::uint64_t m_entryBinding = 0;
    // What admitted the write that installed the gateway. Kept because
    // recovery restores the bytes that write left behind, and the decision
    // covering those bytes is the one taken when they were written. Asking the
    // manifest again would ask about whatever is on disk at that later moment.
    //
    // This holds the decision belonging to the write only because both paths
    // call admits() immediately before the one operation that can leave
    // recovery required. Admitting in one place and writing in another would
    // leave this naming an admission that wrote nothing. It is a field of the
    // manager and its lifetime is the manager's, where what it describes is a
    // single write; the decision belongs on the record of that write, which is
    // where the registry split puts it.
    std::optional<runtime_agent::CoverageDecision> m_admitted;
};
