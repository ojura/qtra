#pragma once

// Stopping every thread in this process, and checking where each of them
// stands, before bytes at a function's entry are overwritten.
//
// The policies beside this one either assert that nothing else runs or refuse.
// That covers a process patched before its threads start and nothing else, so a
// running multi-threaded application could be told no and never yes. This is
// the answer to that: stop them all, ask each one where it is, and write only
// when none of them is inside the bytes about to change.
//
// How they are stopped. Each thread is sent a signal, and the handler parks it
// on a flag until the write is done. The handler records the instruction
// pointer it interrupted, which is the fact the whole policy turns on: a thread
// parked at an address outside the range cannot be executing anything in the
// range, because it is executing nothing at all.
//
// What it cannot do, and says so instead of guessing:
//
//   A thread inside a blocking system call runs the handler when the call
//   returns or is interrupted. One that never returns never arrives, so
//   acquiring has a deadline and refuses when it passes.
//
//   A thread created while this is arranging itself would not be signalled. The
//   task list is read again once everyone has arrived, and a thread that
//   appeared in between makes this refuse, because it was never asked.
//
//   The instruction pointer says where a thread is, not where it has been. A
//   thread standing outside the range with a return address inside it is a
//   thread this cannot see. That is the same limitation the coverage question
//   answers separately, by establishing which calls a replacement reaches.
//
// The handler runs on threads doing arbitrary work, so it touches nothing but
// atomics and makes no library call that could allocate or lock.

#include "agent/quiescence.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace runtime_agent {

// Where each thread was standing when it parked.
struct ParkedThread {
    int tid = 0;
    const void* instructionPointer = nullptr;
};

class StopTheWorldQuiescer final : public Quiescer {
public:
    // The signal used to park threads, and how long to wait for all of them.
    //
    // A real-time signal by default, because those are not used by the C
    // library for anything of its own and are queued rather than merged. An
    // application already using one for its own purposes should say so.
    explicit StopTheWorldQuiescer(int signalNumber = 0,
                                  std::chrono::milliseconds deadline
                                      = std::chrono::milliseconds{500}) noexcept;

    [[nodiscard]] std::unique_ptr<QuiescenceLease> acquire(const WriteRegion& region,
                                                           std::string& error) override;

    [[nodiscard]] const char* name() const noexcept override { return "stop-the-world"; }

    // Where every other thread was standing, from the last acquire, whether it
    // succeeded or not. Empty when nothing was stopped.
    [[nodiscard]] const std::vector<ParkedThread>& parked() const noexcept { return m_parked; }

private:
    int m_signal;
    std::chrono::milliseconds m_deadline;
    std::vector<ParkedThread> m_parked;
};

// The threads this process has right now, by task id.
//
// Read from the kernel's own list, so it counts threads that no library knows
// about. Empty when it cannot be read, which is not the same as a process with
// no threads and is why the caller is told which happened.
[[nodiscard]] bool currentThreadIds(std::vector<int>& tids, std::string& error);

} // namespace runtime_agent
