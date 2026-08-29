#pragma once

// Owns what is installed at an entry and which replacement it currently names.
// Knows nothing about the application it patches: it is handed a resolved site,
// an untyped replacement address, and something that can stop execution.

#include "agent/entry_hotpatch.h"
#include "agent/gateway_record.h"
#include "agent/patch_site.h"
#include "agent/quiescence.h"

#include <memory>
#include <optional>
#include <string>

namespace runtime_agent {

// The entry's own condition, which is separate from what is driving the
// application. A gateway naming its continuation runs the original function, so
// an entry can be rewritten and the original still be what executes.
//
// There is no way back to NoGateway once a gateway is installed. Removing it
// would need another code write with all the same hazards, and a caller asking
// to stop a replacement is not asking for that. The one exception is recovery
// from an install that never finished, which restores the saved bytes while the
// lease that stopped execution is still held.
enum class PatchState {
    NoGateway,
    GatewayOriginal,
    GatewayReplacement,
    RecoveryRequired,
};

[[nodiscard]] const char* describe(PatchState state) noexcept;

struct PatchStatus {
    PatchState state = PatchState::NoGateway;
    std::optional<PatchSite> site;
    void* replacement = nullptr;
    void* slotAddress = nullptr;
};

class PatchManager final {
public:
    PatchManager() = default;
    PatchManager(const PatchManager&) = delete;
    PatchManager& operator=(const PatchManager&) = delete;

    // Writes nothing. A manager holding a lease because an install could not be
    // finished would otherwise resume execution by being destroyed, which is
    // the one thing that state exists to prevent.
    ~PatchManager();

    // Selects which code the entry reaches.
    //
    // The first call installs the gateway, which is one code write with
    // execution stopped. Every later call is a store to the slot: atomic, and
    // needing no quiescence, because a thread reading the slot gets one address
    // or the other.
    [[nodiscard]] bool activate(const PatchSite& site,
                                void* replacement,
                                Quiescer& quiescer,
                                std::string& error);

    // Points the slot back at the continuation, so the entry runs the original
    // function again. The gateway stays installed.
    [[nodiscard]] bool rollback(Quiescer& quiescer, std::string& error);

    [[nodiscard]] PatchStatus status() const;
    [[nodiscard]] PatchState state() const noexcept { return m_state; }

    // Whether a replacement is currently selected. An installed gateway naming
    // its continuation is not.
    [[nodiscard]] bool replacementSelected() const noexcept
    {
        return m_state == PatchState::GatewayReplacement;
    }

    // Tests substitute the permission call to reach the path where the copy has
    // happened and putting the mapping back has not.
    void setProtectFunction(const ProtectFunction protect) noexcept { m_protect = protect; }

private:
    [[nodiscard]] bool installGateway(const PatchSite& site,
                                      Quiescer& quiescer,
                                      std::string& error);

    PatchState m_state = PatchState::NoGateway;
    ProtectFunction m_protect = nullptr;

    // Allocated when the gateway is installed and never moved or freed: the
    // gateway holds the slot's address as an immediate, and a thread can be
    // between the load and the jump at any instant.
    std::unique_ptr<GatewayRecord> m_record;

    // The bytes the prepared area held before the gateway went in, kept so an
    // install that could not be finished can be undone.
    std::vector<std::uint8_t> m_original;

    // Held only while an install has changed bytes and could not finish. For as
    // long as it exists, execution stays stopped.
    std::unique_ptr<QuiescenceLease> m_recoveryLease;
};

} // namespace runtime_agent
