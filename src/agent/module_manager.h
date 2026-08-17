#pragma once

#include "agent/agent_abi.h"
#include "agent/entry_hotpatch.h"
#include "demo/cube_step_abi.h"

#include <QJsonArray>
#include <QJsonObject>
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
