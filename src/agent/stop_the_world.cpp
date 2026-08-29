#include "agent/stop_the_world.h"

#include <atomic>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

namespace runtime_agent {
namespace {

// Everything a handler touches. Lock-free is checked, not assumed: a handler
// that blocked on a lock inside an atomic would be the deadlock this exists to
// avoid, written by the compiler.
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(std::atomic<const void*>::is_always_lock_free);

constexpr std::size_t maxParked = 1024;

// Which stop a handler belongs to.
//
// A signal sent to a thread that never accepted it stays pending, and arrives
// whenever that thread next becomes interruptible. That can be long after the
// stop it belonged to was abandoned. Every handler compares the generation it
// wakes into against the one it was sent for, so a late arrival counts for
// nothing and parks nobody.
std::atomic<unsigned> generation{0};
std::atomic<unsigned> arrivals{0};
std::atomic<unsigned> departures{0};
std::atomic<bool> released{true};
std::atomic<bool> arranging{false};

// One slot per parked thread, written by that thread alone.
std::atomic<const void*> parkedAt[maxParked];
std::atomic<int> parkedTid[maxParked];
std::atomic<unsigned> nextSlot{0};

int currentTid() noexcept
{
    return static_cast<int>(::syscall(SYS_gettid));
}

// Runs on a thread doing arbitrary work. It allocates nothing, takes no lock,
// and calls nothing that might do either. Even yielding is done by a direct
// system call: std::this_thread::yield is not promised to be safe here.
void parkHandler(int signalNumber, siginfo_t* info, void* contextPointer)
{
    // Whose stop this is. si_value carries it, set when the signal was queued.
    const unsigned mine = info != nullptr
        ? static_cast<unsigned>(info->si_value.sival_int)
        : 0U;
    if (!arranging.load(std::memory_order_acquire)
        || mine != generation.load(std::memory_order_acquire)) {
        // A signal left over from a stop that has finished. Its sender is gone
        // and nobody is counting it.
        (void)signalNumber;
        return;
    }

    const void* at = nullptr;
    if (contextPointer != nullptr) {
        auto* context = static_cast<ucontext_t*>(contextPointer);
#if defined(__x86_64__)
        at = reinterpret_cast<const void*>(context->uc_mcontext.gregs[REG_RIP]);
#endif
    }

    const unsigned slot = nextSlot.fetch_add(1, std::memory_order_relaxed);
    if (slot < maxParked) {
        parkedAt[slot].store(at, std::memory_order_relaxed);
        parkedTid[slot].store(currentTid(), std::memory_order_relaxed);
    }

    arrivals.fetch_add(1, std::memory_order_release);

    while (!released.load(std::memory_order_acquire)) {
        (void)::syscall(SYS_sched_yield);
    }

    // Counted on the way out as well as in. A handler still returning is still
    // inside this stop, and the next one must not begin underneath it.
    departures.fetch_add(1, std::memory_order_release);
}

// The thread ids this process has, read into storage the caller already owns.
//
// Read with system calls only, because this runs while other threads are
// parked and one of them may be holding the allocator's lock or the loader's.
// opendir and readdir take both.
bool readThreadIds(int* into, std::size_t capacity, std::size_t& count, int& failure) noexcept
{
    count = 0;
    failure = 0;
    const int directory = static_cast<int>(
        ::syscall(SYS_open, "/proc/self/task", O_RDONLY | O_DIRECTORY, 0));
    if (directory < 0) {
        failure = errno;
        return false;
    }

    // getdents64's own record layout, which is stable and documented.
    struct LinuxDirent64 {
        std::uint64_t inode;
        std::int64_t offset;
        unsigned short recordLength;
        unsigned char type;
        char name[1];
    };

    alignas(8) char buffer[8192];
    bool ok = true;
    for (;;) {
        const long got = ::syscall(SYS_getdents64, directory, buffer, sizeof(buffer));
        if (got < 0) {
            failure = errno;
            ok = false;
            break;
        }
        if (got == 0) {
            break;
        }
        for (long offset = 0; offset < got;) {
            auto* entry = reinterpret_cast<LinuxDirent64*>(buffer + offset);
            offset += entry->recordLength;
            if (entry->name[0] == '.') {
                continue;
            }
            int value = 0;
            for (const char* digit = entry->name; *digit >= '0' && *digit <= '9'; ++digit) {
                value = value * 10 + (*digit - '0');
            }
            if (count < capacity) {
                into[count] = value;
            }
            ++count;
        }
    }
    (void)::syscall(SYS_close, directory);
    return ok && count <= capacity;
}

bool sameThreads(const int* a, std::size_t aCount, const int* b, std::size_t bCount) noexcept
{
    if (aCount != bCount) {
        return false;
    }
    // Membership, not order: the kernel lists them in whatever order it likes,
    // and one thread exiting while another starts keeps the count identical.
    for (std::size_t i = 0; i < aCount; ++i) {
        bool found = false;
        for (std::size_t j = 0; j < bCount && !found; ++j) {
            found = a[i] == b[j];
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

class ParkedLease final : public QuiescenceLease {
public:
    ~ParkedLease() override
    {
        released.store(true, std::memory_order_release);

        // Every handler has to be out before the next stop may begin. One still
        // returning would otherwise be caught by the next generation clearing
        // released, and park in a stop nobody sent it to.
        while (departures.load(std::memory_order_acquire) < m_expected) {
            (void)::syscall(SYS_sched_yield);
        }
        arranging.store(false, std::memory_order_release);
    }

    unsigned m_expected = 0;
};

} // namespace

bool currentThreadIds(std::vector<int>& tids, std::string& error)
{
    int storage[maxParked + 1];
    std::size_t count = 0;
    int failure = 0;
    if (!readThreadIds(storage, maxParked + 1, count, failure)) {
        error = std::string("could not read this process's thread list: ")
            + std::strerror(failure);
        return false;
    }
    tids.assign(storage, storage + count);
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

    // The handler stays installed for the life of the process, so a signal that
    // arrives after this stop is abandoned finds a handler that recognises it
    // as late and returns. Restoring the old disposition while a signal may
    // still be pending would let the default action run, and for a real-time
    // signal the default action ends the process.
    static std::atomic<bool> handlerInstalled{false};
    if (!handlerInstalled.load(std::memory_order_acquire)) {
        struct sigaction parking {};
        parking.sa_sigaction = &parkHandler;
        parking.sa_flags = SA_SIGINFO | SA_RESTART;
        ::sigfillset(&parking.sa_mask);
        if (::sigaction(m_signal, &parking, nullptr) != 0) {
            const int failure = errno;
            arranging.store(false, std::memory_order_release);
            error = std::string("could not install the parking handler: ")
                + std::strerror(failure);
            return nullptr;
        }
        handlerInstalled.store(true, std::memory_order_release);
    }

    // Everything the stopped phase needs, allocated before anything stops. A
    // parked thread may hold the allocator's lock, so allocating after that is
    // a deadlock waiting for a thread that is waiting for this one.
    m_parked.reserve(maxParked);
    int before[maxParked + 1];
    int after[maxParked + 1];
    std::size_t beforeCount = 0;
    std::size_t afterCount = 0;
    int failure = 0;

    auto giveUp = [&](std::string why) -> std::unique_ptr<QuiescenceLease> {
        released.store(true, std::memory_order_release);
        arranging.store(false, std::memory_order_release);
        error = std::move(why);
        return nullptr;
    };

    if (!readThreadIds(before, maxParked + 1, beforeCount, failure)) {
        return giveUp(std::string("could not read this process's thread list: ")
                      + std::strerror(failure));
    }

    const int self = currentTid();
    unsigned others = 0;
    for (std::size_t i = 0; i < beforeCount; ++i) {
        if (before[i] != self) {
            ++others;
        }
    }
    if (beforeCount > maxParked) {
        return giveUp("this process has more threads than this policy can account for");
    }

    const unsigned mine = generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    arrivals.store(0, std::memory_order_relaxed);
    departures.store(0, std::memory_order_relaxed);
    nextSlot.store(0, std::memory_order_relaxed);
    released.store(false, std::memory_order_release);

    auto lease = std::make_unique<ParkedLease>();
    lease->m_expected = others;

    const int pid = ::getpid();
    // The whole structure, because that is what the system call takes. si_code
    // has to say the signal was queued by a process, or the kernel refuses it.
    siginfo_t carried{};
    carried.si_signo = m_signal;
    carried.si_code = SI_QUEUE;
    carried.si_pid = pid;
    carried.si_uid = static_cast<uid_t>(::getuid());
    carried.si_value.sival_int = static_cast<int>(mine);
    for (std::size_t i = 0; i < beforeCount; ++i) {
        if (before[i] == self) {
            continue;
        }
        // Queued with the generation, so a handler can tell a signal meant for
        // this stop from one left over.
        if (::syscall(SYS_rt_tgsigqueueinfo, pid, before[i], m_signal, &carried) != 0) {
            if (errno == ESRCH) {
                continue;
            }
            return giveUp("could not signal a thread");
        }
    }

    const auto until = std::chrono::steady_clock::now() + m_deadline;
    while (arrivals.load(std::memory_order_acquire) < others) {
        if (std::chrono::steady_clock::now() >= until) {
            // The lease releases whoever did arrive and waits for them to
            // leave, so the abandoned stop does not overlap the next one.
            lease->m_expected = arrivals.load(std::memory_order_acquire);
            lease.reset();
            error = "not every thread stopped before the deadline. A thread inside a "
                    "blocking call arrives when the call returns, and one that never "
                    "returns never arrives";
            return nullptr;
        }
        (void)::syscall(SYS_sched_yield);
    }

    if (!readThreadIds(after, maxParked + 1, afterCount, failure)) {
        lease.reset();
        return giveUp(std::string("could not read this process's thread list: ")
                      + std::strerror(failure));
    }
    if (!sameThreads(before, beforeCount, after, afterCount)) {
        lease.reset();
        return giveUp("the threads changed while this was stopping them, so at least one "
                      "was never asked where it stands");
    }

    const unsigned recorded = nextSlot.load(std::memory_order_relaxed);
    if (recorded != others) {
        lease.reset();
        return giveUp("a different number of threads reported in than were asked");
    }
    for (unsigned i = 0; i < recorded; ++i) {
        m_parked.push_back(ParkedThread{parkedTid[i].load(std::memory_order_relaxed),
                                        parkedAt[i].load(std::memory_order_relaxed)});
    }

    for (const ParkedThread& thread : m_parked) {
        if (region.contains(thread.instructionPointer)) {
            const int standing = thread.tid;
            lease.reset();
            return giveUp("thread " + std::to_string(standing)
                          + " is standing inside the bytes about to change, so writing them "
                            "would change the instruction it is about to execute");
        }
    }

    return lease;
}

} // namespace runtime_agent
