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
#include <unordered_map>
#include <vector>

namespace runtime_agent {

class PatchRegistry final {
public:
    explicit PatchRegistry(std::shared_ptr<TextWriter> writer = processTextWriter())
        : m_write(std::move(writer))
    {
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
    [[nodiscard]] PatchManager& forEntry(void* entry);

    // Whether anything is known about this entry yet, which is a question about
    // the registry and not about the entry's condition.
    [[nodiscard]] bool knows(void* entry) const;

    // Every entry a manager has been made for, for reporting.
    [[nodiscard]] std::vector<void*> entries() const;

private:
    std::shared_ptr<TextWriter> m_write;
    std::unordered_map<void*, std::unique_ptr<PatchManager>> m_managers;
};

} // namespace runtime_agent
