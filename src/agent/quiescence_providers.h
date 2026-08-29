#pragma once

// Ready-made answers to "is it safe to write this entry now".
//
// Which one applies is policy, and policy belongs to whoever is orchestrating,
// so the adapter picks exactly one and hands it over. A manager that tried
// several in turn would make both the failures and the side effects harder to
// account for.

#include "agent/quiescence.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace runtime_agent {

// Threads this process currently has, or nothing when that cannot be read.
//
// Absent rather than zero: no live process has zero threads, so a count of zero
// would be an error stored in a field meant for an answer, and every caller
// would have to know that.
//
// Evidence rather than proof. One thread means nothing else can be inside the
// function; more than one only means the question is open, since those threads
// may never touch it.
[[nodiscard]] std::optional<std::size_t> observedThreadCount();

// Nothing is running that could reach the target, asserted by the caller.
//
// For a process patched before the threads that would call it exist, and for a
// test that is the only thread there is. It restores nothing because it stopped
// nothing. A caller reaching for this loosely is claiming something the host has
// no way to check.
class AlreadyQuiescent final : public Quiescer {
public:
    [[nodiscard]] std::unique_ptr<QuiescenceLease> acquire(std::string& error) override;
    [[nodiscard]] const char* name() const noexcept override { return "already-quiescent"; }
};

// The same claim, checked. Succeeds only when this process has exactly one
// thread, which is the one asking.
class SingleThreadQuiescer final : public Quiescer {
public:
    [[nodiscard]] std::unique_ptr<QuiescenceLease> acquire(std::string& error) override;
    [[nodiscard]] const char* name() const noexcept override { return "single-thread"; }
};

// Refuses, and says why.
//
// For a process with threads nobody can account for and no policy that covers
// them. Refusing is the useful answer: the alternative is writing several bytes
// into code another thread may be executing, and the caller can act on a refusal
// by installing earlier.
class RefusingQuiescer final : public Quiescer {
public:
    [[nodiscard]] std::unique_ptr<QuiescenceLease> acquire(std::string& error) override;
    [[nodiscard]] const char* name() const noexcept override { return "refusing"; }
};

} // namespace runtime_agent
