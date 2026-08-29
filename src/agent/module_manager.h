#pragma once

#include "agent/agent_abi.h"
#include "agent/patch_manager.h"
#include "demo/cube_step_abi.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <memory>
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
        // it fails as a race inside the process rather than as an error anyone
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

    [[nodiscard]] bool activateEntryPatch(quint64 id, QString& error);
    [[nodiscard]] bool rollback(QString& error);

    // Binding a replacement on behalf of a loaded module. Returns 0, or the
    // negative code the host ABI documents.
    [[nodiscard]] int bindReplacement(void* target,
                                      void* replacement,
                                      quint64 owner,
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
};
