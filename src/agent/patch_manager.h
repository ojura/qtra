#pragma once

// Owns what is installed at an entry and which replacement it currently names.
// Knows nothing about the application it patches: it is handed a resolved site,
// an untyped replacement address, and something that can stop execution.

#include "agent/entry_hotpatch.h"
#include "agent/gateway_record.h"
#include "agent/patch_site.h"
#include "agent/prologue_relocation.h"
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
// to stop a replacement is not asking for that.
enum class PatchState {
    NoGateway,
    GatewayOriginal,
    GatewayReplacement,
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

enum class PatchUnbindResult {
    Released,
    NotLive,
    WrongOwner,
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

    // Whether the page the gateway is on is still writable, because making it
    // executable again failed after the bytes were copied.
    //
    // The gateway itself is complete and correct: the write that changed the
    // bytes either copies all of them or none. So this is an entry that works
    // and a permission that should be put back, which needs no code write and
    // nothing stopped.
    bool mappingLeftWritable = false;

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
    // manager does. What a gateway leaves behind outlasts everything that asked
    // for the install.
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
    // A write that changes the bytes and cannot make the mapping executable
    // again leaves a complete, working gateway on a page that is still
    // writable. That is reported and is not a failure: nothing here copies part
    // of an instruction stream, so there is no half-written entry to recover
    // from, and putting the permissions back needs no code write and no stop.
    [[nodiscard]] bool installGateway(const PatchSite& site,
                                      const LiveTextWriteAdmission& admission,
                                      Quiescer& quiescer,
                                      std::string& error);

    // Installs at an entry that reserved no space, by moving its opening
    // instructions and writing a jump that reads its destination from a word
    // placed near it.
    //
    // The same arrangement installGateway leaves behind, reached a different
    // way. Afterwards nothing here can tell them apart: choosing what runs is
    // one aligned store into a word, releasing falls back to what ran before,
    // and a replacement chains by calling what the word held. So bind, unbind
    // and the generation rules are shared and not repeated.
    //
    // What differs is what the original is. A prepared entry continues into the
    // rest of its own function; this one continues into a copy of the
    // instructions that were moved, which then continues into the rest.
    [[nodiscard]] bool installRelocatedGateway(const ProloguePlan& plan,
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
    [[nodiscard]] PatchUnbindResult unbindWithResult(std::uint64_t id,
                                                     std::uint64_t owner,
                                                     std::string& error);

    // Convenience for callers that only distinguish success from failure. The
    // adapter uses unbindWithResult so ABI results do not depend on error prose.
    [[nodiscard]] bool unbind(std::uint64_t id, std::uint64_t owner, std::string& error);

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

    // What the gateway was written under, kept for reporting.
    std::optional<LiveTextWriteAdmission> m_installedUnder;

    // What a relocated install left behind, when the entry was reached that
    // way. Kept because the copy stays mapped and its address is what a
    // replacement chains to.
    std::optional<RelocatedPrologue> m_relocated;

    // Whether the page the gateway sits on is still writable, because putting
    // its permissions back failed after the bytes were copied. The gateway
    // itself is complete: nothing here copies part of one.
    bool m_mappingLeftWritable = false;

    std::unique_ptr<GatewayRecord> m_record;
    std::optional<std::size_t> m_threadsAtInstall;
    std::vector<Generation> m_generations;
    std::uint64_t m_nextBinding = 1;
};

} // namespace runtime_agent
