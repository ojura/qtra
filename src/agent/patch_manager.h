#pragma once

// Owns what is installed at an entry and which replacement it currently names.
// Knows nothing about the application it patches: it is handed a resolved site,
// an untyped replacement address, and something that can stop execution.

#include "agent/entry_hotpatch.h"
#include "agent/gateway_record.h"
#include "agent/patch_site.h"
#include "agent/quiescence.h"
#include "agent/write_admission.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
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

    // What the write that installed the gateway was admitted under, from the
    // moment it happened. Absent before anything is written.
    std::optional<LiveTextWriteAdmission> installedUnder;
    // Absent when the count could not be read, which is a different thing from
    // a process with no threads.
    std::optional<std::size_t> threadsAtInstall;

    // What admitted the write that left this entry needing recovery, when it
    // does. Absent otherwise, because there is no write to attribute.
    std::optional<LiveTextWriteAdmission> recoveryAdmission;

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
    // Refuses a null writer in every build. An assert would leave the release
    // build dereferencing it on the first write, which is the build where a
    // failed install matters most.
    explicit PatchManager(std::shared_ptr<TextWriter> writer = processTextWriter())
        : m_write(std::move(writer))
    {
        if (m_write == nullptr) {
            throw std::invalid_argument("a patch manager needs something to write with");
        }
    }

    PatchManager(const PatchManager&) = delete;
    PatchManager& operator=(const PatchManager&) = delete;

    // Writes nothing, and does not free the gateway's record. A manager holding
    // a lease because an install could not be finished would otherwise resume
    // execution by being destroyed, which is the one thing that state exists to
    // prevent, and freeing the record would leave the installed gateway loading
    // from released storage.
    ~PatchManager();

    // Writes the gateway into the entry. The one operation here that changes
    // bytes, so it is the only one that takes what admitted a write and the
    // only one that needs execution stopped.
    //
    // Valid only with no gateway. A replacement is not required: installing
    // early, before anything wants to replace the function, is a write and
    // nothing else, and there is no coverage question to ask yet.
    //
    // Failing after the bytes changed leaves a recovery record holding the
    // saved bytes, the lease, this admission and the writer, which is what
    // makes finishing possible without whoever asked still being here.
    [[nodiscard]] bool installGateway(const PatchSite& site,
                                      const LiveTextWriteAdmission& admission,
                                      Quiescer& quiescer,
                                      std::string& error);

    // Selects which code the entry reaches, and records who asked.
    //
    // Requires a gateway, and writes no bytes: one aligned store into the slot,
    // where every call reads the old destination or the new one. So it takes no
    // admission and no quiescer. A caller reading this line knows no text is at
    // stake, which is what separating it from installation is for.
    //
    // owner identifies the module the binding belongs to, so a release can be
    // checked against it and so nothing can unbind somebody else's work.
    [[nodiscard]] bool bind(void* replacement,
                            std::uint64_t owner,
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

    // The newest binding still live, or nothing when they have all gone.
    [[nodiscard]] const Generation* newestLive() const noexcept;
    void publishSelection() noexcept;

    PatchState m_state = PatchState::NoGateway;
    std::shared_ptr<TextWriter> m_write;

    // What the gateway was written under, whatever became of that write. The
    // record keeps its own required copy, because recovery must not depend on
    // anything outside itself; both are copies of one value made at one
    // instant, so neither can drift from the other.
    std::optional<LiveTextWriteAdmission> m_installedUnder;

    // What an install that changed bytes and could not finish left behind, and
    // everything needed to finish it.
    //
    // Built whole or not at all, so there is no way to hold recovery state
    // without the admission the write was made under, and no way to hold it
    // without something to write with. The manager is in RecoveryRequired
    // exactly when this exists, so the state and its evidence cannot disagree.
    struct Recovery {
        Recovery(std::vector<std::uint8_t> bytes,
                 std::unique_ptr<QuiescenceLease> held,
                 LiveTextWriteAdmission admitted,
                 std::shared_ptr<TextWriter> writer)
            : original(std::move(bytes))
            , lease(std::move(held))
            , admission(std::move(admitted))
            , write(std::move(writer))
        {
        }

        std::vector<std::uint8_t> original;
        std::unique_ptr<QuiescenceLease> lease;
        // Const, so custody cannot be edited after the write it describes.
        const LiveTextWriteAdmission admission;
        std::shared_ptr<TextWriter> write;
    };

    std::unique_ptr<GatewayRecord> m_record;
    std::optional<std::size_t> m_threadsAtInstall;
    std::vector<Generation> m_generations;
    std::uint64_t m_nextBinding = 1;

    std::unique_ptr<Recovery> m_recovery;
};

} // namespace runtime_agent
