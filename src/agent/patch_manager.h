#pragma once

// Owns which function is redirected where, and the state that survives a write
// going wrong. Knows nothing about the application it patches: it is handed a
// resolved site, an untyped replacement address, and something that can stop
// execution long enough to write.

#include "agent/entry_hotpatch.h"
#include "agent/patch_site.h"
#include "agent/quiescence.h"

#include <memory>
#include <optional>
#include <string>

namespace runtime_agent {

struct PatchStatus {
    PatchState state = PatchState::Inactive;
    std::optional<PatchSite> site;
    void* replacement = nullptr;
};

class PatchManager final {
public:
    PatchManager() = default;
    PatchManager(const PatchManager&) = delete;
    PatchManager& operator=(const PatchManager&) = delete;

    // Writes nothing. A manager holding a lease because an install could not be
    // finished would otherwise resume execution by being destroyed, which is
    // the one thing that state exists to prevent. Shut down by rolling back.
    ~PatchManager();

    // Called activate because installing and selecting are the same operation
    // today and separate ones later: the first write puts an encoding at the
    // entry, and what a caller asks for is which replacement runs.
    [[nodiscard]] bool activate(const PatchSite& site,
                                void* replacement,
                                Quiescer& quiescer,
                                std::string& error);

    [[nodiscard]] bool rollback(Quiescer& quiescer, std::string& error);

    [[nodiscard]] PatchStatus status() const;
    [[nodiscard]] bool active() const noexcept { return m_entry.active(); }

    // Tests reach the encoding backend to substitute its permission call.
    void setProtectFunction(const ProtectFunction protect) noexcept
    {
        m_entry.setProtectFunction(protect);
    }

private:
    EntryHotpatch m_entry;
    std::optional<PatchSite> m_site;
    void* m_replacement = nullptr;

    // Held only while an install has changed bytes and could not finish. For as
    // long as it exists, execution stays stopped.
    std::unique_ptr<QuiescenceLease> m_recoveryLease;
};

} // namespace runtime_agent
