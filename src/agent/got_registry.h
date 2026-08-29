#pragma once

// Who owns a redirected slot, and what happens when several want it.
//
// The functions in got_site.h are one store each, over a site the caller holds.
// That is enough to redirect a call and not enough for two callers, or one
// caller twice, or a caller that lets go while another is still using it:
//
//   Resolving again after a redirect reads the replacement and records it as
//   the original, so restoring from that site installs the replacement for
//   good.
//
//   Restoring from an older site overwrites a newer redirect, and the module
//   that made the newer one has no way to know.
//
//   Nothing says whose a redirect is, so nothing can refuse a release from
//   somebody else.
//
// So a slot has one owner for the life of the process, which captures what the
// loader put there once and never reads it again. Everything after that is a
// generation: an ordered list of what each caller asked for, where the newest
// one still live is what the slot names. Releasing is safe in any order,
// because releasing something that is not selected leaves the slot alone, and
// releasing what is selected falls back to the newest predecessor still live
// and only then to the original.
//
// This is the same shape the patched entries use, for the same reason, and
// deliberately not the same code: an entry's owner also holds saved bytes, a
// quiescence lease and a write admission, none of which a slot has. What they
// share is the ordering rule, which is small enough to say twice and clearer
// than one type serving both.

#include "agent/got_site.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace runtime_agent {

// What a caller gets back when it redirects a slot.
struct GotBinding {
    std::uint64_t id = 0;

    // What the loader put in the slot, which is what a replacement calls to
    // reach the function it replaced. The same for every generation on a slot,
    // because it is captured once.
    void* original = nullptr;

    // What this binding displaced, which is the original when nothing else was
    // selected. A replacement that wants to chain to whoever it displaced calls
    // this instead.
    void* previous = nullptr;
};

// What one slot currently holds, for reporting.
struct GotSlotStatus {
    CallerIdentity caller;
    std::string symbol;
    CallKind kind = CallKind::ProcedureLinkageTable;
    void* original = nullptr;
    void* selected = nullptr;

    // Zero when nothing is selected and the slot holds the original again.
    std::uint64_t selectedBinding = 0;
    std::uint64_t selectedOwner = 0;
    std::size_t liveBindings = 0;

    // Whether a write left the page writable when it should not be. The slot
    // names what it was asked to; the permission is what is wrong, and putting
    // it back needs no store.
    bool mappingLeftWritable = false;
};

// Whether a replacement has to carry a landing pad.
enum class LandingPadRule {
    // Refuse a replacement without one. Every way of reaching a slot's
    // destination is an indirect branch, so this is what works whether or not
    // the process is checking them.
    Required,

    // Accept one without. For a caller that knows its process is not checking
    // and would rather say so than rebuild the replacement.
    NotChecked,
};

class GotRegistry final {
public:
    // What does the permission change is given, so a test can reach the
    // outcome where the slot names the replacement and the page could not be
    // put back. Nothing in a running application passes one.
    explicit GotRegistry(SlotProtectFunction protect = nullptr) noexcept
        : m_protect(protect)
    {
    }

    GotRegistry(const GotRegistry&) = delete;
    GotRegistry& operator=(const GotRegistry&) = delete;

    // The one every caller shares, allocated once and never destroyed.
    //
    // What it holds is referred to by slots in loaded objects, which outlive
    // anything that asked for a redirect. Destroying it at exit would drop the
    // originals while those slots still name replacements.
    [[nodiscard]] static GotRegistry& instance();

    // Points a slot at a replacement, on behalf of an owner.
    //
    // The first call for a slot captures what the loader put there. Later calls
    // add a generation and leave that capture alone, so resolving again after a
    // redirect cannot record a replacement as the original.
    //
    // owner is whatever the caller uses to identify itself, and is what a
    // release is checked against.
    [[nodiscard]] bool bind(const GotSite& site,
                            void* replacement,
                            std::uint64_t owner,
                            LandingPadRule rule,
                            GotBinding& binding,
                            std::string& error);

    // Releases one binding.
    //
    // Safe in any order. One that is not selected leaves the slot alone; one
    // that is selects the newest predecessor still live, and the original when
    // there is none.
    [[nodiscard]] bool unbind(std::uint64_t id, std::uint64_t owner, std::string& error);

    // What every slot this has bound currently holds.
    [[nodiscard]] std::vector<GotSlotStatus> status() const;

    // What one slot holds, or nothing where this has never bound it.
    [[nodiscard]] bool statusOf(void** slot, GotSlotStatus& into) const;

private:
    struct Generation {
        std::uint64_t id = 0;
        std::uint64_t owner = 0;
        void* replacement = nullptr;
        bool released = false;
    };

    struct Slot {
        GotSite site;
        void* original = nullptr;
        bool mappingLeftWritable = false;
        std::vector<Generation> generations;

        [[nodiscard]] const Generation* newestLive() const noexcept;
    };

    [[nodiscard]] bool publish(Slot& slot, std::string& error);

    SlotProtectFunction m_protect = nullptr;
    mutable std::mutex m_mutex;
    std::unordered_map<void**, std::unique_ptr<Slot>> m_slots;
    std::uint64_t m_nextBinding = 1;
};

} // namespace runtime_agent
