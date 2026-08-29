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
#include <pthread.h>
#include <sys/stat.h>
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

// Waits for a count of started threads to arrive, or gives up and says so.
//
// Every one of these waits was an unbounded yield loop. When a count never
// arrived the process did not fail, it spun: one was found still running after
// three hours, holding four cores and outliving the binary it had been built
// from. A test that cannot proceed should say so and let the run go red, not
// occupy the machine until somebody goes looking.
//
// Giving up records a failure and returns, rather than exiting early, because
// the shutdown after each of these clears the flag the workers watch. Leaving
// by another route would strand them in their loops and hang the join instead.
bool started(const std::atomic<int>& counter, const int expected, const char* what)
{
    constexpr std::chrono::seconds limit{30};
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (counter.load(std::memory_order_acquire) < expected) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::printf("  FAIL %s: %d of %d started within %lld seconds\n", what,
                        counter.load(std::memory_order_acquire), expected,
                        static_cast<long long>(limit.count()));
            ++failures;
            return false;
        }
        std::this_thread::yield();
    }
    return true;
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

std::atomic<bool> keepBlocking{true};
std::atomic<int> blocking{0};

// Blocks the parking signal and then waits. A signal sent to this thread would
// sit in its queue until it unblocks, which is what the policy refuses to do.
void blockAndWait()
{
    sigset_t mask;
    ::sigemptyset(&mask);
    ::sigaddset(&mask, SIGRTMIN + 3);
    ::pthread_sigmask(SIG_BLOCK, &mask, nullptr);

    blocking.fetch_add(1, std::memory_order_release);
    while (keepBlocking.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

// The inode of a task's own directory under /proc.
//
// The same number the policy pairs with a thread id. Read here separately, so
// the test is asking the kernel and not believing the policy.
unsigned long long taskInodeOf(const int tid)
{
    char path[64] = {};
    std::snprintf(path, sizeof(path), "/proc/self/task/%d", tid);
    struct stat about {};
    if (::stat(path, &about) != 0) {
        return 0;
    }
    return static_cast<unsigned long long>(about.st_ino);
}

// Whether a signal is sitting in a thread's own queue, asked of the kernel.
//
// The test asks this directly instead of believing the policy about it, since
// what is being checked is whether the policy queued anything at all.
bool signalIsQueued(const int tid, const int signalNumber)
{
    char path[64] = {};
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/status", tid);
    std::FILE* status = std::fopen(path, "re");
    if (status == nullptr) {
        return false;
    }
    const unsigned long long bit = 1ULL << (signalNumber - 1);
    char line[256];
    bool queued = false;
    while (std::fgets(line, sizeof(line), status) != nullptr) {
        unsigned long long mask = 0;
        if (std::sscanf(line, "SigPnd: %llx", &mask) == 1) {
            queued = (mask & bit) != 0ULL;
            break;
        }
    }
    (void)std::fclose(status);
    return queued;
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
        (void)started(spinning, workers, "four spinning threads");

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
        (void)started(tight, 1, "the tight spinner");

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
        (void)started(spinning, 1, "the spinner");

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

    std::printf("what makes two readings of the thread list comparable\n");
    {
        // A thread id is a number the kernel reuses, so the list is compared by
        // id and by the inode of the task's own directory. If that inode came
        // back the same for every thread, the comparison would be looking at
        // the ids alone and saying nothing.
        //
        // The moment a task started is the other obvious candidate and does not
        // work: the kernel reports it in clock ticks, and threads created one
        // after another here all report the same one. That is why the inode is
        // the thing paired.
        keepSpinning.store(true, std::memory_order_release);
        spinning.store(0, std::memory_order_release);
        std::thread first(&spin);
        (void)started(spinning, 1, "the spinner");
        std::thread second(&spin);
        (void)started(spinning, 2, "both spinners");

        std::vector<int> tids;
        std::string listing;
        check(runtime_agent::currentThreadIds(tids, listing), "the threads can be listed");

        std::vector<unsigned long long> inodes;
        for (const int tid : tids) {
            inodes.push_back(taskInodeOf(tid));
        }
        bool everyoneHasOne = !inodes.empty();
        for (const unsigned long long inode : inodes) {
            if (inode == 0) {
                everyoneHasOne = false;
            }
        }
        check(everyoneHasOne, "and each one has a task inode");

        bool allDistinct = true;
        for (std::size_t i = 0; i < inodes.size() && allDistinct; ++i) {
            for (std::size_t j = i + 1; j < inodes.size() && allDistinct; ++j) {
                allDistinct = inodes[i] != inodes[j];
            }
        }
        check(allDistinct, "and no two threads share one, so a reused id is still told apart");

        if (!tids.empty()) {
            check(taskInodeOf(tids.front()) == inodes.front(),
                  "and the same thread reads the same both times");
        }

        keepSpinning.store(false, std::memory_order_release);
        first.join();
        second.join();
    }

    std::printf("a thread that blocks the signal\n");
    {
        // Nothing is sent. A signal queued to a thread that blocks it waits
        // there until that thread unblocks, and every attempt would add another
        // against a limit this user's other processes share.
        keepBlocking.store(true, std::memory_order_release);
        blocking.store(0, std::memory_order_release);
        std::thread worker(&blockAndWait);
        (void)started(blocking, 1, "the thread with the parking signal blocked");

        std::vector<int> tids;
        std::string listing;
        (void)runtime_agent::currentThreadIds(tids, listing);

        runtime_agent::StopTheWorldQuiescer quiescer;
        std::string refusal;
        auto denied = quiescer.acquire(
            runtime_agent::WriteRegion{reinterpret_cast<void*>(&quietCorner), 16}, refusal);
        check(denied == nullptr, "is refused");
        check(refusal.find("has this signal blocked") != std::string::npos,
              refusal.empty() ? "with a reason" : refusal.c_str());

        // And nothing was queued to anyone, which is the point of refusing
        // before sending instead of timing out afterwards.
        bool anythingQueued = false;
        for (const int tid : tids) {
            if (signalIsQueued(tid, SIGRTMIN + 3)) {
                anythingQueued = true;
            }
        }
        check(!anythingQueued, "with nothing left sitting in anybody's queue");

        keepBlocking.store(false, std::memory_order_release);
        worker.join();
    }

    std::printf("a deadline that expires, and what it leaves behind\n");
    {
        // Threads that will arrive given any time at all, and a deadline that
        // gives them none. What is tested is the giving up and the state it
        // leaves, not the waiting.
        constexpr int workers = 8;
        keepSpinning.store(true, std::memory_order_release);
        spinning.store(0, std::memory_order_release);
        std::vector<std::thread> threads;
        threads.reserve(workers);
        for (int i = 0; i < workers; ++i) {
            threads.emplace_back(&spin);
        }
        (void)started(spinning, workers, "four spinning threads");

        // Observed and not assumed. A deadline of nothing times out unless
        // every thread arrives between the last signal being sent and the first
        // look at the clock, which is a few microseconds; several attempts are
        // allowed so a scheduler that manages it once does not fail this.
        bool sawTimeout = false;
        std::string refusal;
        for (int attempt = 0; attempt < 50 && !sawTimeout; ++attempt) {
            runtime_agent::StopTheWorldQuiescer impatient(0, std::chrono::milliseconds{0});
            std::string why;
            auto denied = impatient.acquire(
                runtime_agent::WriteRegion{reinterpret_cast<void*>(&quietCorner), 16}, why);
            if (denied == nullptr && why.find("before the deadline") != std::string::npos) {
                sawTimeout = true;
                refusal = why;
            }
        }
        check(sawTimeout, sawTimeout ? refusal.c_str()
                                     : "no attempt with no time at all ever gave up");

        // The mechanism still works afterwards. An abandoned stop leaves its
        // signals queued, and the next one refuses while they are still there
        // and succeeds once they have drained, so a timeout costs a delay and
        // not the ability to stop anything again.
        bool recovered = false;
        std::string lastWhy;
        for (int attempt = 0; attempt < 500 && !recovered; ++attempt) {
            runtime_agent::StopTheWorldQuiescer quiescer;
            std::string why;
            auto lease = quiescer.acquire(
                runtime_agent::WriteRegion{reinterpret_cast<void*>(&quietCorner), 16}, why);
            if (lease != nullptr) {
                recovered = quiescer.parked().size() == workers;
                lease.reset();
            } else {
                lastWhy = why;
                std::this_thread::yield();
            }
        }
        check(recovered, recovered
                  ? "and a later stop accounts for every thread again"
                  : (lastWhy.empty() ? "the mechanism never recovered" : lastWhy.c_str()));

        keepSpinning.store(false, std::memory_order_release);
        for (std::thread& thread : threads) {
            thread.join();
        }
    }

    std::printf("two stops at once\n");
    {
        // One at a time. A second would advance the generation and reset the
        // counters under the first, and the first's handlers would then be
        // counted by a stop they were never sent for.
        keepSpinning.store(true, std::memory_order_release);
        spinning.store(0, std::memory_order_release);
        std::thread worker(&spin);
        (void)started(spinning, 1, "the spinner");

        runtime_agent::StopTheWorldQuiescer first;
        std::string error;
        auto held = first.acquire(
            runtime_agent::WriteRegion{reinterpret_cast<void*>(&quietCorner), 16}, error);
        check(held != nullptr, error.empty() ? "the first stop holds" : error.c_str());

        runtime_agent::StopTheWorldQuiescer second;
        std::string refusal;
        auto denied = second.acquire(
            runtime_agent::WriteRegion{reinterpret_cast<void*>(&quietCorner), 16}, refusal);
        check(denied == nullptr, "and a second while it is held is refused");
        check(refusal.find("already arranged") != std::string::npos,
              refusal.empty() ? "with a reason" : refusal.c_str());

        held.reset();

        // The refusal was about the first still holding and not about anything
        // it broke: once released, a stop works again.
        runtime_agent::StopTheWorldQuiescer third;
        std::string after;
        auto again = third.acquire(
            runtime_agent::WriteRegion{reinterpret_cast<void*>(&quietCorner), 16}, after);
        check(again != nullptr, after.empty() ? "and the next one after it succeeds"
                                              : after.c_str());
        check(third.parked().size() == 1, "accounting for the worker again");
        again.reset();

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

    std::printf("something installed over the parking handler\n");
    {
        // Last, because this leaves the signal reaching somebody else's handler
        // and the policy refusing from then on. That is the correct answer and
        // there is nothing here that could put the parking handler back.
        struct sigaction intruder {};
        intruder.sa_handler = [](int) {};
        ::sigemptyset(&intruder.sa_mask);
        (void)::sigaction(SIGRTMIN + 3, &intruder, nullptr);

        runtime_agent::StopTheWorldQuiescer quiescer;
        std::string refusal;
        auto denied = quiescer.acquire(
            runtime_agent::WriteRegion{reinterpret_cast<void*>(&quietCorner), 16}, refusal);
        check(denied == nullptr, "is refused instead of signalling into it");
        check(refusal.find("installed over the parking handler") != std::string::npos,
              refusal.empty() ? "with a reason" : refusal.c_str());
    }

    std::printf("%s\n",
                failures == 0 ? "all thread accounting checks passed"
                              : "thread accounting checks failed");
    return failures == 0 ? 0 : 1;
}
