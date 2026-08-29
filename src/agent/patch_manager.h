#pragma once

// Owns what is installed at an entry and which replacement it currently names.
// Knows nothing about the application it patches: it is handed a resolved site,
// an untyped replacement address, and something that can stop execution.

#include "agent/entry_hotpatch.h"
#include "agent/gateway_record.h"
#include "agent/patch_site.h"
#include "agent/quiescence.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

// What a caller gets back when it binds a replacement.
struct PatchBinding {
    std::uint64_t id = 0;

    // Reaches the original function without going through the gateway again, so
    // a replacement can call it. Under CET it is a valid landing pad.
    void* original = nullptr;

    // What this binding displaced, which is the original when nothing else was
    // selected. A replacement that wants to chain calls this.
    void* previous = nullptr;
};

struct PatchStatus {
    PatchState state = PatchState::NoGateway;
    std::optional<PatchSite> site;
    void* replacement = nullptr;
    void* slotAddress = nullptr;

    // What made the one code write safe, and what was observed at the time.
    // A thread count above one is not proof the write was safe; it is what the
    // policy was judged against.
    std::string quiescedBy;
    // Absent when the count could not be read, which is a different thing from
    // a process with no threads.
    std::optional<std::size_t> threadsAtInstall;

    // Whose replacement is selected. Zero when the original is running.
    std::uint64_t selectedBinding = 0;
    std::uint64_t selectedOwner = 0;
    std::size_t liveBindings = 0;
};

class PatchManager final {
public:
    // What does the writing is given, so a test can reach the state where the
    // copy has happened and the mapping has not been put back without the
    // product carrying a way to ask for that.
    // Shares ownership of its writer, so the writer cannot go before the
    // manager does. Recovery is a write, and what needs recovering can outlast
    // everything that asked for the install.
    explicit PatchManager(std::shared_ptr<TextWriter> writer = processTextWriter())
        : m_write(std::move(writer))
    {
        assert(m_write != nullptr && "a patch manager needs something to write with");
    }

    PatchManager(const PatchManager&) = delete;
    PatchManager& operator=(const PatchManager&) = delete;

    // Writes nothing, and does not free the gateway's record. A manager holding
    // a lease because an install could not be finished would otherwise resume
    // execution by being destroyed, which is the one thing that state exists to
    // prevent, and freeing the record would leave the installed gateway loading
    // from released storage.
    ~PatchManager();

    // Selects which code the entry reaches, and records who asked.
    //
    // The first call installs the gateway, which is one code write with
    // execution stopped. Every later call is a store to the slot: atomic, and
    // needing no quiescence, because a thread reading the slot gets one address
    // or the other.
    //
    // owner identifies the module the binding belongs to, so a release can be
    // checked against it and so nothing can unbind somebody else's work.
    [[nodiscard]] bool bind(const PatchSite& site,
                            void* replacement,
                            std::uint64_t owner,
                            Quiescer& quiescer,
                            PatchBinding& binding,
                            std::string& error);

    // Releases one binding.
    //
    // Bindings form a stack, and releasing out of order is safe: a binding that
    // is not the selected one leaves the slot alone, and one that is selects the
    // nearest predecessor still live. Without that, an old module letting go
    // would overwrite a replacement chosen after it.
    [[nodiscard]] bool unbind(std::uint64_t id, std::uint64_t owner, std::string& error);

    // Puts the entry back to its own bytes. Only possible while recovering from
    // an install that never finished, which still holds the lease that stopped
    // execution.
    [[nodiscard]] bool recover(std::string& error);

    [[nodiscard]] PatchStatus status() const;
    [[nodiscard]] PatchState state() const noexcept { return m_state; }

    [[nodiscard]] bool replacementSelected() const noexcept
    {
        return m_state == PatchState::GatewayReplacement;
    }

    // Where a replacement reaches the original without re-entering the gateway.
    [[nodiscard]] void* continuation() const noexcept;

private:
    // One replacement somebody bound. Kept for the life of the process: the code
    // it names is in a module that is never unloaded, and a thread can be
    // between the slot load and the jump at any instant.
    struct Generation {
        std::uint64_t id = 0;
        std::uint64_t owner = 0;
        void* replacement = nullptr;
        bool released = false;
    };

    [[nodiscard]] bool installGateway(const PatchSite& site,
                                      Quiescer& quiescer,
                                      std::string& error);

    // The newest binding still live, or nothing when they have all gone.
    [[nodiscard]] const Generation* newestLive() const noexcept;
    void publishSelection() noexcept;

    PatchState m_state = PatchState::NoGateway;
    std::shared_ptr<TextWriter> m_write;

    std::unique_ptr<GatewayRecord> m_record;
    std::vector<std::uint8_t> m_original;
    std::string m_quiescedBy;
    std::optional<std::size_t> m_threadsAtInstall;
    std::vector<Generation> m_generations;
    std::uint64_t m_nextBinding = 1;

    std::unique_ptr<QuiescenceLease> m_recoveryLease;
};

} // namespace runtime_agent
