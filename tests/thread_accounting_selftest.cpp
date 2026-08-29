// Whether every thread was accounted for before bytes at an entry changed.
//
// The other policies assert that nothing else runs, or refuse. Neither can say
// yes to a running multi-threaded process, so this one stops every thread and
// asks each where it stands. The tests here are about that claim: that all of
// them really stop, that where they stand is really read, and that a thread
// inside the bytes is a refusal and not a footnote.

#include "agent/stop_the_world.h"

#include <atomic>
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

} // namespace

int main()
{
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
        // The thread parks inside the spin loop, so the loop's own address
        // range is where it is standing. Asking about that range has to refuse.
        keepSpinning.store(true, std::memory_order_release);
        spinning.store(0, std::memory_order_release);
        std::thread worker(&spin);
        while (spinning.load(std::memory_order_acquire) < 1) {
            std::this_thread::yield();
        }

        std::string error;
        runtime_agent::StopTheWorldQuiescer locating;
        auto found = locating.acquire(
            runtime_agent::WriteRegion{reinterpret_cast<void*>(&quietCorner), 16}, error);
        check(found != nullptr, "the worker stopped so its position could be read");
        const void* where = locating.parked().empty()
            ? nullptr
            : locating.parked().front().instructionPointer;
        found.reset();

        check(where != nullptr, "and the position was read");
        if (where != nullptr) {
            runtime_agent::StopTheWorldQuiescer refusing;
            std::string refusal;
            auto denied = refusing.acquire(runtime_agent::WriteRegion{where, 8}, refusal);
            check(denied == nullptr, "writing where a thread stands is refused");
            check(refusal.find("standing inside") != std::string::npos,
                  refusal.empty() ? "with a reason" : refusal.c_str());
        }

        keepSpinning.store(false, std::memory_order_release);
        worker.join();
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
