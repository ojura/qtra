#pragma once

// Who owns the state behind a patched entry, and for how long.
//
// A gateway written into a function's entry loads its destination from a slot,
// and that instruction stays in the text for as long as the process runs. The
// storage the slot lives in can therefore never be reclaimed, and the same
// holds for the bytes saved from an install that could not finish and for the
// lease that install is still holding.
//
// So the owner has to outlive every caller that asks for a patch. A registry
// that does makes that ownership instead of a leak: nothing has to remember not
// to destroy a manager, because nothing holds one to destroy.
//
// This owns entry state and nothing else. Loaded modules have their own
// lifetime, decided by who may still call into them, and putting the two in one
// object would tie each to the other's rules.

#include "agent/entry_hotpatch.h"
#include "agent/patch_manager.h"

#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace runtime_agent {

class PatchRegistry final {
public:
    // Refused here as well as in PatchManager, so a registry cannot be built
    // that will fail later on whichever entry is asked for first.
    explicit PatchRegistry(std::shared_ptr<TextWriter> writer = processTextWriter())
        : m_write(std::move(writer))
    {
        if (m_write == nullptr) {
            throw std::invalid_argument("a patch registry needs something to write with");
        }
    }

    PatchRegistry(const PatchRegistry&) = delete;
    PatchRegistry& operator=(const PatchRegistry&) = delete;

    // The one registry the application uses, allocated once and never
    // destroyed.
    //
    // Never destroyed on purpose. Running the destructor at exit would take
    // the managers with it, and a manager still holding a recovery lease would
    // release it, which is the one thing that state exists to prevent. Nothing
    // else in the process is still running by then to benefit from the memory.
    [[nodiscard]] static PatchRegistry& instance();

    // The manager for this entry, made on the first ask and kept afterwards.
    //
    // Keyed by the entry, so two callers naming the same function get the same
    // manager and cannot install two gateways over each other. A caller holds
    // the reference for as long as it likes; it belongs to the registry.
    //
    // Safe to call from any thread: the map is locked, and the reference stays
    // valid however the map grows afterwards, because managers are separate
    // allocations.
    //
    // The lock covers finding and making a manager and nothing more. What a
    // manager does once handed out, and what its status reports, are not
    // guarded here, so one thread drives patching. That is a requirement and
    // not an oversight: a write into an entry has to happen on a thread allowed
    // to write it, which no lock in this class could establish.
    [[nodiscard]] PatchManager& forEntry(void* entry);

    // Whether anything is known about this entry yet, which is a question about
    // the registry and not about the entry's condition.
    [[nodiscard]] bool knows(void* entry) const;

    // Every entry a manager has been made for, for reporting.
    [[nodiscard]] std::vector<void*> entries() const;

private:
    // Guards the map only. What a manager does once handed out is the caller's
    // to order: a write into an entry has to happen on a thread that may write
    // it, which is a stronger requirement than any lock here could express.
    mutable std::mutex m_mutex;
    std::shared_ptr<TextWriter> m_write;
    std::unordered_map<void*, std::unique_ptr<PatchManager>> m_managers;
};

} // namespace runtime_agent
