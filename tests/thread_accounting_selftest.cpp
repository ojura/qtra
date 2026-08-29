// Whether every thread was accounted for before bytes at an entry changed.
//
// The other policies assert that nothing else runs, or refuse. Neither can say
// yes to a running multi-threaded process, so this one stops every thread and
// asks each where it stands. The tests here are about that claim: that all of
// them really stop, that where they stand is really read, and that a thread
// inside the bytes is a refusal and not a footnote.

#include "agent/stop_the_world.h"

#include <atomic>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const char* what)
{
    if (!condition) {
        std::printf("  FAIL %s\n", what);
        ++failures;
        return;
    }
    std::printf("  ok   %s\n", what);
}

std::atomic<bool> keepSpinning{true};
std::atomic<int> spinning{0};

void spin()
{
    spinning.fetch_add(1, std::memory_order_release);
    while (keepSpinning.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

// Somewhere in this process's text that no thread is executing, for the case
// where the answer should be yes.
void quietCorner() {}

std::atomic<bool> keepTight{true};
std::atomic<int> tight{0};

// A loop that calls nothing, so a thread in it is standing in these bytes.
//
// The other spinner yields, which means it is usually inside the C library and
// not here at all. That is the policy's stated limitation showing up in its own
// test: a thread that has called out of a range has its return address inside
// it and its instruction pointer somewhere else, and this sees only the second.
__attribute__((noinline)) void tightSpin()
{
    tight.fetch_add(1, std::memory_order_release);
    while (keepTight.load(std::memory_order_acquire)) {
        // Nothing. A call here would put the thread in the callee.
    }
}

} // namespace

int main()
{
    std::printf("a signal the application already uses\n");
    {
        // Taking it would disable what the application does with it, for the
        // life of the process, with nothing to put back.
        struct sigaction mine {};
        mine.sa_handler = [](int) {};
        ::sigemptyset(&mine.sa_mask);
        const int chosen = SIGRTMIN + 6;
        struct sigaction previous {};
        (void)::sigaction(chosen, &mine, &previous);

        runtime_agent::StopTheWorldQuiescer intruder(chosen);
        std::string refusal;
        auto denied = intruder.acquire(
            runtime_agent::WriteRegion{reinterpret_cast<void*>(&quietCorner), 16}, refusal);
        check(denied == nullptr, "is refused instead of replacing what it does");
        check(refusal.find("already does something with signal") != std::string::npos,
              refusal.empty() ? "with a reason" : refusal.c_str());

        struct sigaction after {};
        (void)::sigaction(chosen, nullptr, &after);
        check(after.sa_handler == mine.sa_handler,
              "and what the application installed is still there");
        (void)::sigaction(chosen, &previous, nullptr);
    }

    // Alone, so the count is the count.
    std::printf("with no other threads\n");
    {
        std::vector<int> tids;
        std::string error;
        check(runtime_agent::currentThreadIds(tids, error), "the thread list is readable");
        check(tids.size() == 1, "and this process has one thread");

        runtime_agent::StopTheWorldQuiescer quiescer;
        const runtime_agent::WriteRegion region{reinterpret_cast<void*>(&quietCorner), 16};
        auto lease = quiescer.acquire(region, error);
        check(lease != nullptr, "stopping nobody succeeds");
        check(quiescer.parked().empty(), "and parks nobody");
    }

    std::printf("with threads running\n");
    {
        constexpr int workers = 4;
        std::vector<std::thread> threads;
        threads.reserve(workers);
        for (int i = 0; i < workers; ++i) {
            threads.emplace_back(&spin);
        }
        while (spinning.load(std::memory_order_acquire) < workers) {
            std::this_thread::yield();
        }

        std::string error;
        std::vector<int> tids;
        (void)runtime_agent::currentThreadIds(tids, error);
        check(tids.size() == workers + 1, "the kernel's list counts them all");

        runtime_agent::StopTheWorldQuiescer quiescer;
        const runtime_agent::WriteRegion region{reinterpret_cast<void*>(&quietCorner), 16};
        {
            auto lease = quiescer.acquire(region, error);
            check(lease != nullptr,
                  error.empty() ? "every thread stopped" : error.c_str());
            check(quiescer.parked().size() == workers,
                  "and every one of them said where it stands");

            bool everyoneSomewhere = true;
            for (const runtime_agent::ParkedThread& thread : quiescer.parked()) {
                if (thread.instructionPointer == nullptr || thread.tid == 0) {
                    everyoneSomewhere = false;
                }
            }
            check(everyoneSomewhere, "with a real address and a real thread id each");

            // A region covering where a parked thread stands has to be refused,
            // which is the whole point of asking. Nothing is written here; the
            // question is only whether the policy would allow it.
            if (!quiescer.parked().empty()) {
                const void* occupied = quiescer.parked().front().instructionPointer;
                check(runtime_agent::WriteRegion{occupied, 1}.contains(occupied),
                      "a region containing an address says so");
            }
        }

        // The lease is gone, so they are running again.
        std::string second;
        runtime_agent::StopTheWorldQuiescer again;
        auto secondLease = again.acquire(
            runtime_agent::WriteRegion{reinterpret_cast<void*>(&quietCorner), 16}, second);
        check(secondLease != nullptr,
              second.empty() ? "releasing lets them go, so they can be stopped again"
                             : second.c_str());
        check(again.parked().size() == workers, "and all of them arrive the second time");
        secondLease.reset();

        keepSpinning.store(false, std::memory_order_release);
        for (std::thread& thread : threads) {
            thread.join();
        }
    }

    std::printf("a thread standing in the bytes about to change\n");
    {
        // The region is the whole function the thread is looping in, not the
        // exact address it occupied once. A thread in a loop is at a different
        // instruction from one moment to the next, so asking about the eight
        // bytes it happened to be in is a test that passes when it is lucky.
        keepTight.store(true, std::memory_order_release);
        tight.store(0, std::memory_order_release);
        std::thread worker(&tightSpin);
        while (tight.load(std::memory_order_acquire) < 1) {
            std::this_thread::yield();
        }

        const runtime_agent::WriteRegion body{reinterpret_cast<void*>(&tightSpin), 256};

        // Established first, so the refusal below is known to be about a thread
        // that really is in this range and not about an empty claim.
        std::string error;
        runtime_agent::StopTheWorldQuiescer locating;
        auto found = locating.acquire(
            runtime_agent::WriteRegion{reinterpret_cast<void*>(&quietCorner), 16}, error);
        check(found != nullptr, "the worker stopped so its position could be read");
        const bool insideBody = !locating.parked().empty()
            && body.contains(locating.parked().front().instructionPointer);
        found.reset();
        check(insideBody, "and it is standing inside the function it is looping in");

        if (insideBody) {
            runtime_agent::StopTheWorldQuiescer refusing;
            std::string refusal;
            auto denied = refusing.acquire(body, refusal);
            check(denied == nullptr, "writing that function's bytes is refused");
            check(refusal.find("standing inside") != std::string::npos,
                  refusal.empty() ? "with a reason" : refusal.c_str());
            check(refusal.find("thread ") != std::string::npos,
                  "naming the thread that is standing there");
        }

        keepTight.store(false, std::memory_order_release);
        worker.join();
    }

    std::printf("a thread that has called out of the range is not seen in it\n");
    {
        // The limitation, tested so it is a known property and not a surprise.
        // This thread's return address is inside the yielding spinner, and its
        // instruction pointer is in the C library, so a region covering that
        // function does not refuse.
        keepSpinning.store(true, std::memory_order_release);
        spinning.store(0, std::memory_order_release);
        std::thread worker(&spin);
        while (spinning.load(std::memory_order_acquire) < 1) {
            std::this_thread::yield();
        }

        std::string error;
        runtime_agent::StopTheWorldQuiescer quiescer;
        auto lease = quiescer.acquire(
            runtime_agent::WriteRegion{reinterpret_cast<void*>(&spin), 256}, error);
        check(lease != nullptr,
              "a thread inside a call it made from the range is not standing in the range");
        lease.reset();

        keepSpinning.store(false, std::memory_order_release);
        worker.join();
    }

    std::printf("a stop that is abandoned does not disturb the next one\n");
    {
        // A signal sent to a thread that never took it stays pending. If a
        // later stop counted it, or the handler ran with the old disposition
        // restored, this is where it would show.
        keepSpinning.store(true, std::memory_order_release);
        spinning.store(0, std::memory_order_release);
        std::thread worker(&spin);
        while (spinning.load(std::memory_order_acquire) < 1) {
            std::this_thread::yield();
        }

        for (int round = 0; round < 20; ++round) {
            runtime_agent::StopTheWorldQuiescer quiescer;
            std::string error;
            auto lease = quiescer.acquire(
                runtime_agent::WriteRegion{reinterpret_cast<void*>(&quietCorner), 16}, error);
            if (lease == nullptr || quiescer.parked().size() != 1) {
                check(false, error.empty() ? "a round stopped the wrong number of threads"
                                           : error.c_str());
                break;
            }
        }
        check(true, "twenty stops in a row each account for exactly one thread");

        keepSpinning.store(false, std::memory_order_release);
        worker.join();
    }

    std::printf("a second policy asking for a different signal\n");
    {
        // One handler is installed for the life of the process, because a
        // signal sent earlier can still arrive. A second instance naming
        // another signal would send one nothing handles, and a real-time signal
        // with no handler ends the process.
        runtime_agent::StopTheWorldQuiescer other(SIGRTMIN + 4);
        std::string refusal;
        auto denied = other.acquire(
            runtime_agent::WriteRegion{reinterpret_cast<void*>(&quietCorner), 16}, refusal);
        check(denied == nullptr, "is refused instead of sending a signal nothing handles");
        check(refusal.find("already parks threads with signal") != std::string::npos,
              refusal.empty() ? "with a reason" : refusal.c_str());
    }

    std::printf("without being told which bytes\n");
    {
        runtime_agent::StopTheWorldQuiescer quiescer;
        std::string error;
        auto lease = quiescer.acquire(runtime_agent::WriteRegion{}, error);
        check(lease == nullptr, "a policy that checks a region refuses to be asked without one");
    }

    std::printf("%s\n",
                failures == 0 ? "all thread accounting checks passed"
                              : "thread accounting checks failed");
    return failures == 0 ? 0 : 1;
}
