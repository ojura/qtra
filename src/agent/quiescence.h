#pragma once

// Stopping whatever might run a function while its entry is being written.
//
// Writing a redirect takes more than one instruction's worth of bytes, and a
// thread whose next instruction is inside that area while it changes will
// execute whatever it finds. Who can be stopped, and how, is a property of the
// application: this demo pauses one animation timer, a general target needs
// every thread accounted for, and a target patched before its threads start
// needs nothing at all.
//
// A lease is held for exactly as long as execution has to stay stopped, and
// destroying it restores what the provider stopped. That is what an
// enter-and-leave pair could not offer: an early return leaves nothing halted,
// and a caller with a reason to keep execution stopped, such as an install that
// changed bytes and could not finish, keeps the lease instead of remembering to
// suppress a call.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace runtime_agent {

// The bytes about to change.
//
// Every policy here answers one question, which is whether these particular
// bytes can be written now. Saying which bytes is what lets a policy that can
// actually account for threads check that none of them is standing in the
// range, and asking without saying would be asking something weaker than the
// caller needs. A policy that stops nothing has no use for it.
struct WriteRegion {
    const void* start = nullptr;
    std::size_t bytes = 0;

    // Compared as integers. Relational operators on pointers into different
    // objects are not defined, and these two are exactly that: an address a
    // thread happened to stop at, and the start of some other range.
    [[nodiscard]] bool contains(const void* address) const noexcept
    {
        if (start == nullptr || bytes == 0) {
            return false;
        }
        const auto at = reinterpret_cast<std::uintptr_t>(address);
        const auto from = reinterpret_cast<std::uintptr_t>(start);
        // A range that wraps past the top of the address space describes
        // nothing real, so it contains nothing.
        if (from > std::numeric_limits<std::uintptr_t>::max() - bytes) {
            return false;
        }
        return at >= from && at < from + bytes;
    }
};

// Execution is stopped while this exists.
class QuiescenceLease {
public:
    QuiescenceLease() = default;
    QuiescenceLease(const QuiescenceLease&) = delete;
    QuiescenceLease& operator=(const QuiescenceLease&) = delete;

    // Restores whatever acquiring it stopped, and only that: a provider that
    // found execution already stopped leaves it stopped.
    virtual ~QuiescenceLease() = default;
};

class Quiescer {
public:
    Quiescer() = default;
    Quiescer(const Quiescer&) = delete;
    Quiescer& operator=(const Quiescer&) = delete;
    virtual ~Quiescer() = default;

    // Returns nothing and sets error when execution cannot be stopped, which is
    // a refusal to patch and not a reason to write anyway.
    [[nodiscard]] virtual std::unique_ptr<QuiescenceLease> acquire(const WriteRegion& region,
                                                                   std::string& error) = 0;

    // Which policy this is, recorded with the install so a caller can tell
    // what made the write safe. "the timer was stopped" and "there was only
    // one thread" are different claims and a result that says neither is not
    // worth much.
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    // Whether the lease may be held after the write it was taken for.
    //
    // An install that changed bytes and could not finish keeps its lease, so
    // nothing reaches an entry that is half of two things. That works for a
    // policy whose lease costs the rest of the process nothing.
    //
    // It does not work for one that stopped threads where they stood. Those
    // threads are holding whatever they held: the allocator's lock, the
    // loader's, a Qt lock. Returning through ordinary code while they are
    // parked means formatting an error, answering a request or writing a log
    // can wait on a lock only a parked thread can release. The write has to be
    // finished or undone before returning, and a policy that says false here
    // is saying so.
    [[nodiscard]] virtual bool leaseMaySurviveTheWrite() const noexcept { return true; }
};

} // namespace runtime_agent
