#include "agent/stop_the_world.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>

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
// How many handlers are executing, counted before anything else they do and
// uncounted on every way out.
//
// This is what a stop can wait for. Waiting for the signals it sent to arrive
// waits forever when one went to a thread that has the signal blocked: it stays
// pending until that thread unblocks it, which may be never. A handler that is
// running will finish.
std::atomic<unsigned> inside{0};
std::atomic<bool> released{true};

// Two separate facts, which one flag cannot carry.
//
// controllerBusy says a stop owns this machinery, and stays true until every
// handler from it has left. activeGeneration says which stop a handler may
// belong to, and is cleared first so a signal delivered from that moment on is
// recognised as late and returns without touching anything.
//
// With one flag doing both, clearing it to let late signals go also let the
// next stop claim ownership, and an old handler still between its check and its
// arrival could then count itself into the new stop's counters.
std::atomic<bool> controllerBusy{false};
std::atomic<unsigned> activeGeneration{0};

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
    // Counted first, before anything is decided, so a controller waiting for
    // handlers to leave cannot miss one between arriving and being recognised.
    inside.fetch_add(1, std::memory_order_acq_rel);

    // Whose stop this is. si_value carries it, set when the signal was queued.
    const unsigned mine = info != nullptr
        ? static_cast<unsigned>(info->si_value.sival_int)
        : 0U;
    const unsigned active = activeGeneration.load(std::memory_order_acquire);
    if (active == 0U || mine != active) {
        // A signal left over from a stop that has finished. Its sender is gone
        // and nobody is counting it.
        (void)signalNumber;
        inside.fetch_sub(1, std::memory_order_acq_rel);
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

    inside.fetch_sub(1, std::memory_order_acq_rel);
}

// A thread, and something that stays true about it for as long as it exists.
//
// A thread id on its own is a number the kernel reuses. One thread exiting and
// another starting can be given the same one, and a list compared by id alone
// then says two readings describe the same threads when they describe different
// ones.
//
// The number paired here is the inode of the task's own directory under /proc.
// Two tasks alive at the same time never share one, and the kernel hands it out
// from a counter and not from the id, so a reused id comes back with a
// different inode. The listing already carries it, so pairing costs nothing.
//
// The moment a task started is the other obvious candidate and does not work:
// the kernel reports it in clock ticks, so threads created within the same tick
// share a value. Five threads created one after another in a test all reported
// the same one.
struct ThreadIdentity {
    int tid = 0;
    unsigned long long inode = 0;
};

// The thread ids this process has, read into storage the caller already owns.
//
// Read with system calls only, because this runs while other threads are
// parked and one of them may be holding the allocator's lock or the loader's.
// opendir and readdir take both.
bool readThreadIds(ThreadIdentity* into, std::size_t capacity, std::size_t& count,
                   int& failure) noexcept
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
                into[count].tid = value;
                into[count].inode = entry->inode;
            }
            ++count;
        }
    }
    (void)::syscall(SYS_close, directory);
    return ok && count <= capacity;
}

// What a thread does with this signal right now: whether it is blocking it, and
// whether one is already sitting in its queue.
//
// Blocked matters because a signal queued to a thread that blocks it stays
// queued until that thread unblocks it, which may be never. The stop then times
// out, and every retry queues another against a limit this user's other
// processes share.
//
// Pending matters for the same resource, one step later. A stop that timed out
// leaves its signals queued on whichever threads never took them, and those
// entries are spent until delivery. Sending more before they drain is how a few
// timeouts turn into a process that cannot signal anything.
//
// Read with ordinary file calls, which is safe here: this is asked before
// anything is sent, so nothing is parked.
bool readSignalState(const int tid, const int signalNumber, bool& blocked,
                     bool& pending) noexcept
{
    blocked = false;
    pending = false;

    char path[64] = {};
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/status", tid);
    std::FILE* status = std::fopen(path, "re");
    if (status == nullptr) {
        // Gone between listing and asking, which the second reading of the list
        // answers, and a thread that has gone is holding nothing queued. Any
        // other failure means the masks are simply unknown, and guessing them
        // is not answering the question.
        return errno == ENOENT || errno == ESRCH;
    }

    const unsigned long long bit = 1ULL << (signalNumber - 1);
    char line[256];
    bool sawBlocked = false;
    bool sawPending = false;
    while (std::fgets(line, sizeof(line), status) != nullptr) {
        unsigned long long mask = 0;
        if (std::sscanf(line, "SigBlk: %llx", &mask) == 1) {
            blocked = (mask & bit) != 0ULL;
            sawBlocked = true;
        } else if (std::sscanf(line, "SigPnd: %llx", &mask) == 1) {
            pending = (mask & bit) != 0ULL;
            sawPending = true;
        }
        if (sawBlocked && sawPending) {
            break;
        }
    }
    (void)std::fclose(status);
    return sawBlocked && sawPending;
}

// Threads a stop signalled and then abandoned.
//
// Each of those signals is still queued until the thread it went to becomes
// interruptible and the handler recognises it as belonging to a stop that has
// finished. Until then the queue entry is spent, so the next stop asks whether
// they have drained before adding more.
std::atomic<int> outstanding[maxParked];
std::atomic<unsigned> outstandingCount{0};

void rememberOutstanding(const int* tids, const unsigned count) noexcept
{
    const unsigned kept = count < maxParked ? count : maxParked;
    for (unsigned i = 0; i < kept; ++i) {
        outstanding[i].store(tids[i], std::memory_order_relaxed);
    }
    outstandingCount.store(kept, std::memory_order_release);
}

bool sameThreads(const ThreadIdentity* a, std::size_t aCount,
                 const ThreadIdentity* b, std::size_t bCount) noexcept
{
    if (aCount != bCount) {
        return false;
    }
    // Membership, not order: the kernel lists them in whatever order it likes.
    // Both halves have to match, because one thread exiting and another being
    // given its number keeps the count and the number identical while
    // describing a task nobody asked anything of.
    for (std::size_t i = 0; i < aCount; ++i) {
        bool found = false;
        for (std::size_t j = 0; j < bCount && !found; ++j) {
            found = a[i].tid == b[j].tid && a[i].inode == b[j].inode;
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
        // Parked handlers may leave, and this stop stops being one anybody can
        // belong to, so a signal delivered from here on returns without
        // parking.
        released.store(true, std::memory_order_release);
        activeGeneration.store(0U, std::memory_order_release);

        // Then wait for the handlers actually running, and only those. A signal
        // sent to a thread that has it blocked stays pending until that thread
        // unblocks it, which may be never, so waiting for every signal sent to
        // arrive waits for something that need not happen.
        while (inside.load(std::memory_order_acquire) != 0U) {
            (void)::syscall(SYS_sched_yield);
        }

        // Only now may another stop begin. Until every handler from this one
        // has left, one of them could still be counted by the next.
        controllerBusy.store(false, std::memory_order_release);
    }
};

} // namespace

bool currentThreadIds(std::vector<int>& tids, std::string& error)
{
    // On the stack. The statics inside acquire are safe because one stop owns
    // them at a time, and this is a public function with no such guard: two
    // callers would share one buffer with nothing saying they must not. It
    // fills a vector anyway, so shared storage bought nothing here.
    ThreadIdentity storage[maxParked + 1];
    std::size_t count = 0;
    int failure = 0;
    if (!readThreadIds(storage, maxParked + 1, count, failure)) {
        error = std::string("could not read this process's thread list: ")
            + std::strerror(failure);
        return false;
    }
    tids.clear();
    tids.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        tids.push_back(storage[i].tid);
    }
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
    if (!controllerBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        error = "another stop is already arranged; this policy handles one at a time";
        return nullptr;
    }

    // The handler stays installed for the life of the process, so a signal that
    // arrives after this stop is abandoned finds a handler that recognises it
    // as late and returns. Restoring the old disposition while a signal may
    // still be pending would let the default action run, and for a real-time
    // signal the default action ends the process.
    // Which signal the handler is installed for, remembered so a second
    // instance asking for a different one is refused instead of sending a
    // signal nothing handles. A real-time signal with no handler ends the
    // process, so getting this wrong kills whatever it was meant to protect.
    static std::atomic<int> installedFor{0};
    const int already = installedFor.load(std::memory_order_acquire);
    if (already != 0 && already != m_signal) {
        controllerBusy.store(false, std::memory_order_release);
        error = "this process already parks threads with signal " + std::to_string(already)
            + ", and one handler is installed for the life of the process because a signal "
              "sent earlier can still arrive. Use that signal or none";
        return nullptr;
    }
    if (already == 0) {
        // Installed in one call, which hands back exactly what was displaced.
        // Asking first and installing second leaves a gap in which another
        // thread can register the application's own handler, and this would
        // then overwrite it having just been told there was nothing there.
        struct sigaction parking {};
        parking.sa_sigaction = &parkHandler;
        parking.sa_flags = SA_SIGINFO | SA_RESTART;
        ::sigfillset(&parking.sa_mask);

        struct sigaction displaced {};
        if (::sigaction(m_signal, &parking, &displaced) != 0) {
            const int failure = errno;
            controllerBusy.store(false, std::memory_order_release);
            error = std::string("could not install the parking handler: ")
                + std::strerror(failure);
            return nullptr;
        }

        const bool wasTaken = ((displaced.sa_flags & SA_SIGINFO) != 0
                               && displaced.sa_sigaction != nullptr)
            || ((displaced.sa_flags & SA_SIGINFO) == 0
                && displaced.sa_handler != SIG_DFL);
        if (wasTaken) {
            // Put back what was there, then refuse. Parking claims a signal for
            // the life of the process, so taking one the application uses would
            // replace what it does with no way back.
            //
            // This is a repair after the fact and not a check before: sigaction
            // has no install-only-if-unused form, so the handler is already in
            // place by the time its predecessor is known. See the header on why
            // the signal has to be reserved by whoever embeds this.
            const bool restored = ::sigaction(m_signal, &displaced, nullptr) == 0;
            controllerBusy.store(false, std::memory_order_release);
            error = "this process already does something with signal "
                + std::to_string(m_signal)
                + ", and parking threads takes that signal for good, so installing here "
                  "would quietly replace what it does. Choose a signal it does not use";
            if (!restored) {
                error += ". What was there could not be put back either, so this signal now "
                         "reaches a parking handler that no stop will ever use";
            }
            return nullptr;
        }

        installedFor.store(m_signal, std::memory_order_release);
    }

    // Everything the stopped phase needs, allocated before anything stops. A
    // parked thread may hold the allocator's lock, so allocating after that is
    // a deadlock waiting for a thread that is waiting for this one.
    m_parked.reserve(maxParked);
    static ThreadIdentity before[maxParked + 1];
    static ThreadIdentity after[maxParked + 1];
    static int signalled[maxParked];
    std::size_t beforeCount = 0;
    std::size_t afterCount = 0;
    int failure = 0;

    auto giveUp = [&](std::string why) -> std::unique_ptr<QuiescenceLease> {
        released.store(true, std::memory_order_release);
        activeGeneration.store(0U, std::memory_order_release);
        while (inside.load(std::memory_order_acquire) != 0U) {
            (void)::syscall(SYS_sched_yield);
        }
        controllerBusy.store(false, std::memory_order_release);
        error = std::move(why);
        return nullptr;
    };

    // What an earlier stop abandoned, before adding to it. Those signals are
    // still queued until the threads they went to take them.
    const unsigned stillOut = outstandingCount.load(std::memory_order_acquire);
    for (unsigned i = 0; i < stillOut; ++i) {
        const int tid = outstanding[i].load(std::memory_order_relaxed);
        bool blocked = false;
        bool pending = false;
        if (!readSignalState(tid, m_signal, blocked, pending)) {
            return giveUp("a stop was abandoned with a signal queued to thread "
                          + std::to_string(tid)
                          + ", and whether it has been taken since cannot be read, so "
                            "sending more would be adding to a queue of unknown depth");
        }
        if (pending) {
            return giveUp("a stop was abandoned with a signal queued to thread "
                          + std::to_string(tid)
                          + ", and it is still there. Sending another would spend a second "
                            "entry against a limit this user's other processes share, and "
                            "every retry would spend one more. It drains when that thread "
                            "next takes a signal");
        }
    }
    outstandingCount.store(0, std::memory_order_release);

    if (!readThreadIds(before, maxParked + 1, beforeCount, failure)) {
        return giveUp(std::string("could not read this process's threads: ")
                      + std::strerror(failure));
    }

    const int self = currentTid();
    unsigned others = 0;
    for (std::size_t i = 0; i < beforeCount; ++i) {
        if (before[i].tid != self) {
            ++others;
        }
    }
    if (beforeCount > maxParked) {
        return giveUp("this process has more threads than this policy can account for");
    }

    // Before a generation exists, so a refusal here leaves nothing behind.
    for (std::size_t i = 0; i < beforeCount; ++i) {
        if (before[i].tid == self) {
            continue;
        }
        bool blocked = false;
        bool pending = false;
        if (!readSignalState(before[i].tid, m_signal, blocked, pending)) {
            return giveUp("could not read what thread " + std::to_string(before[i].tid)
                          + " does with this signal, so whether it would ever arrive is "
                            "unknown");
        }
        if (blocked) {
            return giveUp("thread " + std::to_string(before[i].tid)
                          + " has this signal blocked, so one sent to it would sit in the "
                            "queue until that thread unblocks it. Sending anyway would time "
                            "out and leave it queued, and every retry would add another "
                            "against a limit this user's other processes share");
        }
    }

    // The handler is claimed once and the process runs on. Something may have
    // installed over it since, and sending then delivers a signal to whatever
    // is there now.
    struct sigaction current {};
    if (::sigaction(m_signal, nullptr, &current) != 0) {
        const int why = errno;
        return giveUp(std::string("could not read what this signal currently reaches: ")
                      + std::strerror(why));
    }
    if ((current.sa_flags & SA_SIGINFO) == 0 || current.sa_sigaction != &parkHandler) {
        return giveUp("something has installed over the parking handler since it was "
                      "claimed, so sending this signal would reach that instead");
    }

    const unsigned mine = generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    arrivals.store(0, std::memory_order_relaxed);
    nextSlot.store(0, std::memory_order_relaxed);
    released.store(false, std::memory_order_release);
    // Published last, so nothing can belong to this stop until everything it
    // will read is in place.
    activeGeneration.store(mine, std::memory_order_release);

    auto lease = std::make_unique<ParkedLease>();

    const int pid = ::getpid();
    // The whole structure, because that is what the system call takes. si_code
    // has to say the signal was queued by a process, or the kernel refuses it.
    siginfo_t carried{};
    carried.si_signo = m_signal;
    carried.si_code = SI_QUEUE;
    carried.si_pid = pid;
    carried.si_uid = static_cast<uid_t>(::getuid());
    carried.si_value.sival_int = static_cast<int>(mine);
    // Counted as they go out, because the lease waits for exactly the threads
    // that were sent one. Abandoning while it still expects a thread that was
    // never signalled waits for a departure that cannot happen.
    unsigned sent = 0;
    for (std::size_t i = 0; i < beforeCount; ++i) {
        if (before[i].tid == self) {
            continue;
        }
        // Queued with the generation, so a handler can tell a signal meant for
        // this stop from one left over.
        if (::syscall(SYS_rt_tgsigqueueinfo, pid, before[i].tid, m_signal, &carried) != 0) {
            if (errno == ESRCH) {
                // Gone between reading the list and signalling it. The list is
                // read again below, which is where that is answered.
                continue;
            }
            rememberOutstanding(signalled, sent);
            lease.reset();
            error = "a thread could not be signalled, so not every one of them could be "
                    "asked where it stands";
            return nullptr;
        }
        signalled[sent] = before[i].tid;
        ++sent;
    }
    others = sent;

    // Worked out before the wait, and read with the system call directly. The
    // first ordinary clock call can need the loader, and by this point threads
    // are parked and one of them may be holding the loader's lock.
    struct timespec deadline {};
    if (::syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &deadline) != 0) {
        rememberOutstanding(signalled, sent);
        lease.reset();
        error = "the clock could not be read, so there is no way to give up on threads that "
                "never arrive";
        return nullptr;
    }
    deadline.tv_sec += m_deadline.count() / 1000;
    deadline.tv_nsec += (m_deadline.count() % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    while (arrivals.load(std::memory_order_acquire) < others) {
        struct timespec now {};
        const bool told =
            ::syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &now) == 0;
        const bool passed = !told
            || now.tv_sec > deadline.tv_sec
            || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec);
        if (passed) {
            // Whoever did not arrive still has this signal queued, and the next
            // stop asks about that before sending any more.
            rememberOutstanding(signalled, sent);
            // The lease releases whoever did arrive and waits for them to
            // leave, so the abandoned stop does not overlap the next one.
            lease.reset();
            error = told
                ? "not every thread stopped before the deadline. A thread inside a blocking "
                  "call arrives when the call returns, and one that never returns never "
                  "arrives"
                : "the clock stopped answering while waiting for threads to stop";
            return nullptr;
        }
        (void)::syscall(SYS_sched_yield);
    }

    // From here the lease exists, and destroying it is the only thing that may
    // clear this stop's state. Calling the pre-lease path as well would write
    // into whatever stop had started in the meantime.
    if (!readThreadIds(after, maxParked + 1, afterCount, failure)) {
        lease.reset();
        error = std::string("could not read this process's threads: ")
            + std::strerror(failure);
        return nullptr;
    }
    if (!sameThreads(before, beforeCount, after, afterCount)) {
        lease.reset();
        error = "the threads changed while this was stopping them, so at least one was "
                "never asked where it stands";
        return nullptr;
    }

    const unsigned recorded = nextSlot.load(std::memory_order_relaxed);
    if (recorded != others) {
        lease.reset();
        error = "a different number of threads reported in than were asked";
        return nullptr;
    }
    for (unsigned i = 0; i < recorded; ++i) {
        m_parked.push_back(ParkedThread{parkedTid[i].load(std::memory_order_relaxed),
                                        parkedAt[i].load(std::memory_order_relaxed)});
    }

    for (const ParkedThread& thread : m_parked) {
        if (region.contains(thread.instructionPointer)) {
            const int standing = thread.tid;
            lease.reset();
            error = "thread " + std::to_string(standing)
                + " is standing inside the bytes about to change, so writing them would "
                  "change the instruction it is about to execute";
            return nullptr;
        }
    }

    return lease;
}

} // namespace runtime_agent
