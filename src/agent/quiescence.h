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

#include <memory>
#include <string>

namespace runtime_agent {

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
    [[nodiscard]] virtual std::unique_ptr<QuiescenceLease> acquire(std::string& error) = 0;
};

} // namespace runtime_agent
