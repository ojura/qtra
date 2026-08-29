#pragma once

#include "agent/agent_abi.h"
#include "agent/coverage_manifest.h"
#include "agent/module_registry.h"
#include "agent/patch_registry.h"
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
    // The cube's adapter onto the generic pieces: it knows this application's
    // patch descriptor, its one target, which thread may write it, and what the
    // window should say. Everything about loading and identifying modules is
    // the registry's, and everything about what an entry holds is the patch
    // manager's.
    using LoadedModule = runtime_agent::ModuleRegistry::LoadedModule;

    explicit ModuleManager(CubeWidget* cube);
    ~ModuleManager();

    ModuleManager(const ModuleManager&) = delete;
    ModuleManager& operator=(const ModuleManager&) = delete;

    [[nodiscard]] LoadedModule* loadSnippet(const QString& path, QString& error)
    {
        return m_registry.loadSnippet(path, error);
    }
    [[nodiscard]] LoadedModule* loadCubePatch(const QString& path, QString& error);
    [[nodiscard]] LoadedModule* module(const quint64 id) const { return m_registry.module(id); }
    [[nodiscard]] QJsonArray list() const { return m_registry.list(); }

    // acceptIncompleteCoverage proceeds when the build could not establish that
    // replacing the target reaches every call. It is a request a caller has to
    // make, because the default is to refuse what nobody has shown to be safe.
    [[nodiscard]] bool activateEntryPatch(quint64 id,
                                          bool acceptIncompleteCoverage,
                                          QString& error);
    // Not const: reporting the evidence in force is the same act as finding
    // it, and a status that read the file separately could say one thing
    // while an activation in the next breath answered to another.
    [[nodiscard]] QJsonObject coverage();

private:
    // Puts the label back in step with what the entry actually reaches. Called
    // after anything that can change that, so the window never shows a claim
    // the manager would contradict. Presentation, so nothing outside calls it.
    void refreshLabel();

    // The one place that decides whether a write may go ahead, so both the
    // protocol and the host binding call answer to the same rules.
    [[nodiscard]] bool admits(bool acceptIncompleteCoverage,
                              runtime_agent::CoverageDecision& decision,
                              QString& error);
    [[nodiscard]] runtime_agent::CoverageDecision readDecision() const;
    [[nodiscard]] runtime_agent::CoverageDecision latestEvidence();
    [[nodiscard]] bool installIfNeeded(const runtime_agent::PatchSite& site,
                                       const runtime_agent::CoverageDecision& decision,
                                       runtime_agent::Quiescer& quiescer,
                                       std::string& error);

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
    [[nodiscard]] QJsonObject patchStatus();

private:
    [[nodiscard]] bool resetActivePatch(QString& error);

    CubeWidget* m_cube = nullptr;
    runtime_agent::ModuleRegistry m_registry;
    // Borrowed from the registry, which outlives this. What a gateway leaves
    // behind is referred to by the process's own text, so destroying this must
    // not take it: an agent torn down and built again finds the same manager
    // with the same gateway and the same recovery state.
    runtime_agent::PatchManager& m_patches;
    // The protocol's own binding and the module it belongs to, kept so
    // rollback can release exactly that one and nothing else. Not what status
    // reports: what the entry reaches is the manager's to answer, and a module
    // binding through the host ABI never touches these.
    quint64 m_activeEntryModule = 0;
    // The protocol's own binding, so releasing it goes through the same
    // generation rule a snippet's binding does.
    std::uint64_t m_entryBinding = 0;
};
