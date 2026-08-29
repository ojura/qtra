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
// A record is allocated when its gateway is installed and never moved or freed,
// which the manager enforces by releasing it and never deleting it. Ownership
// here means who allocated it, never who may reclaim it.
// Code reached through the slot is never unloaded, and a thread can be between
// the load and the jump at any instant, so no moment exists at which reclaiming
// one is provably safe. That matches how loaded modules are already treated.

#include "agent/patch_site.h"

#include <atomic>
#include <cstdint>

namespace runtime_agent {

struct GatewayRecord {
    // The word the gateway loads its destination from, where the gateway can
    // reach this one. A gateway written into a prepared area loads it through a
    // register, which reaches anywhere, so it uses this.
    //
    // Lock-free and pointer-aligned, which is what makes selecting a
    // replacement a single atomic store instead of a code write.
    alignas(sizeof(void*)) std::atomic<void*> slot{nullptr};

    // Where the selection is actually published.
    //
    // Normally this word. An entry with no prepared area is rewritten to a jump
    // that reads its destination from a place named as a distance from itself,
    // which reaches two gigabytes, and this record is wherever the allocator
    // put it. So that install provides a word near the entry and points this at
    // it. Everything that chooses what runs stores through this and does not
    // care which arrangement it is.
    std::atomic<void*>* selection = nullptr;

    [[nodiscard]] std::atomic<void*>& selected() noexcept
    {
        return selection != nullptr ? *selection : slot;
    }

    // Inside the prepared area, immediately after the jump. An ENDBR64 followed
    // by the remaining NOPs, falling through into the function's own
    // instructions, so a call with nothing selected runs the original.
    void* continuation = nullptr;

    PatchSite site;


    // The gateway loads this storage as one pointer word, so its representation
    // has to be exactly that. Both hold on GNU/x86-64 and neither is guaranteed
    // by the standard, which is why they are stated here.
    static_assert(std::atomic<void*>::is_always_lock_free,
                  "the gateway loads this word directly, so it cannot carry a lock");
    static_assert(sizeof(std::atomic<void*>) == sizeof(void*),
                  "the gateway reads one pointer, so the atomic cannot be wider than one");
    static_assert(alignof(std::atomic<void*>) >= sizeof(void*),
                  "an unaligned slot has no atomic store");
};

} // namespace runtime_agent
