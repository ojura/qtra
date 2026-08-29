#include "agent/stop_the_world.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <thread>

#include <dirent.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

namespace runtime_agent {
namespace {

// Shared with the signal handler, so everything here is either atomic or only
// touched while no handler can be running.
//
// One arrangement at a time. Two overlapping stops would have handlers reading
// one set of flags and reporting into another, and a caller wanting that is
// asking for something this cannot describe.
std::atomic<bool> arranging{false};
std::atomic<int> arrivals{0};
std::atomic<bool> released{true};

// Where each parked thread stood. Written by the handlers, one slot each, so no
// two handlers touch the same memory.
constexpr std::size_t maxParked = 1024;
std::atomic<const void*> parkedAt[maxParked];
std::atomic<int> parkedTid[maxParked];
std::atomic<std::size_t> nextSlot{0};

int gettid() noexcept
{
    return static_cast<int>(::syscall(SYS_gettid));
}

// Runs on a thread doing arbitrary work, so it allocates nothing, locks
// nothing, and calls nothing that might.
void parkHandler(int, siginfo_t*, void* contextPointer)
{
    if (!arranging.load(std::memory_order_acquire)) {
        return;
    }

    const void* at = nullptr;
    if (contextPointer != nullptr) {
        auto* context = static_cast<ucontext_t*>(contextPointer);
#if defined(__x86_64__)
        at = reinterpret_cast<const void*>(context->uc_mcontext.gregs[REG_RIP]);
#endif
    }

    const std::size_t slot = nextSlot.fetch_add(1, std::memory_order_relaxed);
    if (slot < maxParked) {
        parkedAt[slot].store(at, std::memory_order_relaxed);
        parkedTid[slot].store(gettid(), std::memory_order_relaxed);
    }

    arrivals.fetch_add(1, std::memory_order_release);

    // Parked until the write is done. Spinning and not waiting on anything,
    // because every way of waiting properly takes a lock this must not take.
    while (!released.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

class ParkedLease final : public QuiescenceLease {
public:
    ~ParkedLease() override
    {
        released.store(true, std::memory_order_release);
        arranging.store(false, std::memory_order_release);
        if (m_restore) {
            (void)::sigaction(m_signal, &m_previous, nullptr);
        }
    }

    int m_signal = 0;
    struct sigaction m_previous {};
    bool m_restore = false;
};

} // namespace

bool currentThreadIds(std::vector<int>& tids, std::string& error)
{
    tids.clear();
    DIR* directory = ::opendir("/proc/self/task");
    if (directory == nullptr) {
        error = std::string("could not read this process's thread list: ")
            + std::strerror(errno);
        return false;
    }
    while (const dirent* entry = ::readdir(directory)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        tids.push_back(std::atoi(entry->d_name));
    }
    (void)::closedir(directory);
    return true;
}

StopTheWorldQuiescer::StopTheWorldQuiescer(const int signalNumber,
                                           const std::chrono::milliseconds deadline) noexcept
    : m_signal(signalNumber != 0 ? signalNumber : SIGRTMIN + 3)
    , m_deadline(deadline)
{
}

std::unique_ptr<QuiescenceLease> StopTheWorldQuiescer::acquire(const WriteRegion& region,
                                                               std::string& error)
{
    m_parked.clear();

    if (region.start == nullptr || region.bytes == 0) {
        error = "this policy checks where threads stand against the bytes about to change, "
                "so it needs to be told what they are";
        return nullptr;
    }

    bool expected = false;
    if (!arranging.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        error = "another stop is already arranged; this policy handles one at a time";
        return nullptr;
    }

    auto giveUp = [&](std::string why) -> std::unique_ptr<QuiescenceLease> {
        released.store(true, std::memory_order_release);
        arranging.store(false, std::memory_order_release);
        error = std::move(why);
        return nullptr;
    };

    std::vector<int> before;
    if (!currentThreadIds(before, error)) {
        return giveUp(error);
    }

    const int self = gettid();
    std::size_t others = 0;
    for (const int tid : before) {
        if (tid != self) {
            ++others;
        }
    }
    if (others >= maxParked) {
        return giveUp("this process has more threads than this policy can account for");
    }

    arrivals.store(0, std::memory_order_relaxed);
    nextSlot.store(0, std::memory_order_relaxed);
    released.store(false, std::memory_order_release);

    struct sigaction parking {};
    parking.sa_sigaction = &parkHandler;
    parking.sa_flags = SA_SIGINFO | SA_RESTART;
    ::sigfillset(&parking.sa_mask);
    struct sigaction previous {};
    if (::sigaction(m_signal, &parking, &previous) != 0) {
        return giveUp(std::string("could not install the parking handler: ")
                      + std::strerror(errno));
    }

    auto lease = std::make_unique<ParkedLease>();
    lease->m_signal = m_signal;
    lease->m_previous = previous;
    lease->m_restore = true;

    const int pid = ::getpid();
    for (const int tid : before) {
        if (tid == self) {
            continue;
        }
        if (::syscall(SYS_tgkill, pid, tid, m_signal) != 0 && errno != ESRCH) {
            return giveUp("could not signal thread " + std::to_string(tid) + ": "
                          + std::strerror(errno));
        }
    }

    const auto until = std::chrono::steady_clock::now() + m_deadline;
    while (static_cast<std::size_t>(arrivals.load(std::memory_order_acquire)) < others) {
        if (std::chrono::steady_clock::now() >= until) {
            return giveUp("only " + std::to_string(arrivals.load(std::memory_order_relaxed))
                          + " of " + std::to_string(others)
                          + " other threads stopped before the deadline. A thread inside a "
                            "blocking call arrives when the call returns, and one that never "
                            "returns never arrives");
        }
        std::this_thread::yield();
    }

    // Asked again, because a thread created while this was arranging itself was
    // never signalled and is running now.
    std::vector<int> after;
    if (!currentThreadIds(after, error)) {
        return giveUp(error);
    }
    if (after.size() != before.size()) {
        return giveUp("the thread list changed while this was stopping them, so at least one "
                      "thread was never asked where it stands");
    }

    const std::size_t recorded =
        std::min(nextSlot.load(std::memory_order_relaxed), maxParked);
    m_parked.reserve(recorded);
    for (std::size_t i = 0; i < recorded; ++i) {
        m_parked.push_back(ParkedThread{parkedTid[i].load(std::memory_order_relaxed),
                                        parkedAt[i].load(std::memory_order_relaxed)});
    }

    for (const ParkedThread& thread : m_parked) {
        if (region.contains(thread.instructionPointer)) {
            return giveUp("thread " + std::to_string(thread.tid)
                          + " is standing inside the bytes about to change, so writing them "
                            "would change the instruction it is about to execute");
        }
    }

    return lease;
}

} // namespace runtime_agent
