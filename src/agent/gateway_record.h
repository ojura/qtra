#pragma once

// Everything one installed gateway owns.
//
// An entry rewritten to jump to a fixed address has to be rewritten again to
// point somewhere else, and that write spans several bytes, cannot be done
// atomically, and needs every thread accounted for. An entry rewritten to jump
// through a slot is written once. Choosing what runs afterwards is a store to
// the slot, and an aligned pointer-sized store on x86-64 is atomic: a thread
// reading it gets the old address or the new one.
//
// So the dangerous operation happens once, when the gateway is installed, and
// every later change is free and needs no quiescence.
//
// A record is allocated when its gateway is installed and never moved or freed.
// Code reached through the slot is never unloaded, and a thread can be between
// the load and the jump at any instant, so no moment exists at which reclaiming
// one is provably safe. That matches how loaded modules are already treated.

#include "agent/patch_site.h"

#include <atomic>
#include <cstdint>

namespace runtime_agent {

struct GatewayRecord {
    // The word the gateway loads its destination from. The gateway holds this
    // address as an immediate, so the object must not move.
    //
    // Lock-free and pointer-aligned, which is what makes selecting a
    // replacement a single atomic store instead of a code write.
    alignas(sizeof(void*)) std::atomic<void*> slot{nullptr};

    // Inside the prepared area, immediately after the jump. An ENDBR64 followed
    // by the remaining NOPs, falling through into the function's own
    // instructions, so a call with nothing selected runs the original.
    void* continuation = nullptr;

    PatchSite site;

    // What the slot currently names, kept for reporting. The slot itself is the
    // truth; this is the manager's record of what it published.
    void* selected = nullptr;

    static_assert(std::atomic<void*>::is_always_lock_free,
                  "the gateway loads this word directly, so it cannot carry a lock");
};

} // namespace runtime_agent
