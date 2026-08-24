#pragma once

#include "agent/agent_abi.h"
#include "agent/entry_hotpatch.h"
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
        const RuntimeAgentSnippetV1* snippet = nullptr;
        const CubeStepPatchV1* cubePatch = nullptr;

        // How this module was last run successfully, which is what its release
        // runs under unless the caller says otherwise. This is observed rather
        // than declared, so it cannot go stale the way a descriptor field would
        // when someone changes the install path and forgets to update it.
        //
        // The last success and not the first attempt: a snippet needing a GL
        // context fails under gui with "use executor=render" and is retried,
        // and a release under the executor that already failed fails the same
        // way. The target is held weakly because it can be gone by release
        // time — a worker that finished, an object already destroyed — and a
        // caller is better told that than silently given a different one.
        QString lastExecutor;
        QPointer<QObject> lastTarget;
        bool hadSuccessfulRun = false;

        // Whether the module carries a release entry point. Descriptors built
        // against the older struct stop after run(), so the size is checked
        // before the field is read.
        [[nodiscard]] bool declaresRelease() const
        {
            return snippet != nullptr
                && snippet->struct_size >= RUNTIME_AGENT_SNIPPET_V1_WITH_RELEASE
                && snippet->release != nullptr;
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

    [[nodiscard]] bool activateDispatchPatch(quint64 id, QString& error);
    [[nodiscard]] bool activateEntryPatch(quint64 id, QString& error);
    [[nodiscard]] bool rollback(QString& error);
    [[nodiscard]] QJsonObject patchStatus() const;

private:
    [[nodiscard]] LoadedModule* insertModule(std::unique_ptr<LoadedModule> module);
    [[nodiscard]] bool resetActivePatch(QString& error);
    [[nodiscard]] static QJsonObject moduleJson(const LoadedModule& module);

    CubeWidget* m_cube = nullptr;
    quint64 m_nextId = 1;
    std::unordered_map<quint64, std::unique_ptr<LoadedModule>> m_modules;
    runtime_agent::EntryHotpatch m_entryHotpatch;
    quint64 m_activeDispatchModule = 0;
    quint64 m_activeEntryModule = 0;
};
