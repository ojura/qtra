// Reaching a function in a library this build does not produce, and owning the
// redirect once it is reached.
//
// The entry patcher needs space reserved at a function's entry, which means
// building the object that defines it. Qt's libraries are not ours to build, so
// nothing that needs preparation reaches them.
//
// A call from one object into another loads its destination from a slot in the
// caller's own table, and the relocation that fills it belongs to the caller. So
// this redirects what Qt calls without Qt being involved, without a byte of
// anybody's code changing, and without stopping anything.
//
// What it does not do, and the reason this is a narrower claim than replacing
// an entry: it redirects the calls one object makes. Calls made inside the
// library that defines the function, or from a third object, still reach the
// original.

#include "agent/got_registry.h"
#include "agent/got_site.h"

#include "no_plt_caller.h"

#include <QByteArray>

#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

int replacementCalls = 0;

// What the slot held before it was redirected, which is what a replacement is
// given to chain to.
using Allocator = void* (*)(std::size_t);
Allocator originalAllocator = nullptr;

// Forwards to what was captured from the slot, and not to malloc by name.
//
// Those need not be the same function. Something may have interposed on the
// allocator this object calls, or it may be resolved in a different namespace,
// so calling by name would forward to whatever this executable resolves and
// quietly test something else. Chaining through what the slot held is the thing
// the backend promises a replacement can do.
extern "C" void* countingMalloc(const std::size_t bytes)
{
    ++replacementCalls;
    return originalAllocator != nullptr ? originalAllocator(bytes) : nullptr;
}

int secondReplacementCalls = 0;

extern "C" void* alsoCountingMalloc(const std::size_t bytes)
{
    ++secondReplacementCalls;
    return originalAllocator != nullptr ? originalAllocator(bytes) : nullptr;
}

// Deliberately without the marker an indirect branch may land on, which the
// rest of this file has because it is built with branch protection.
extern "C" __attribute__((nocf_check)) void* unmarkedMalloc(const std::size_t bytes)
{
    return originalAllocator != nullptr ? originalAllocator(bytes) : nullptr;
}

int measureCalls = 0;

extern "C" std::size_t countingStrlen(const char* text)
{
    ++measureCalls;
    std::size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

// Lets the page be made writable and refuses to put it back, which is the order
// that leaves the slot naming the replacement after the caller has been told the
// write did not finish.
int failRestoreProtect(void* address, std::size_t length, int protection)
{
    if ((protection & PROT_WRITE) != 0) {
        return ::mprotect(address, length, protection);
    }
    errno = EACCES;
    return -1;
}

// Refuses to make the page writable while refusingWrites is set.
//
// A registry keeps its permission call for life, so the switch lives here: the
// binding is made while this behaves, and the release is attempted while it does
// not, which is the order that leaves a slot naming a replacement the registry
// has been asked to let go of.
std::atomic<bool> refusingWrites{false};

int switchableProtect(void* address, std::size_t length, int protection)
{
    if ((protection & PROT_WRITE) != 0 && refusingWrites.load(std::memory_order_acquire)) {
        errno = EACCES;
        return -1;
    }
    return ::mprotect(address, length, protection);
}

const runtime_agent::CallerQuery qtCore =
    runtime_agent::CallerQuery::byName("libQt6Core.so.6");

} // namespace

int main()
{
    runtime_agent::GotRegistry& registry = runtime_agent::GotRegistry::instance();
    std::string error;

    std::printf("a call Qt makes, redirected without rebuilding Qt\n");

    // Warmed before anything is resolved. A slot the loader has not bound yet
    // holds its resolver stub, and resolving refuses that, so asking first
    // would skip this test for a reason that has nothing to do with whether the
    // mechanism works.
    QByteArray warm = QByteArray("warm the allocator").toBase64();
    check(!warm.isEmpty(), "QtCore works before anything is touched");

    runtime_agent::GotSite site;
    if (!runtime_agent::resolveGotSlot(qtCore, "malloc", site, error)) {
        // Skipping is only right where this Qt does not call malloc through a
        // slot at all. Any other failure is this failing, and treating them
        // alike is how a test reports success for never having run.
        std::string listing;
        const std::vector<runtime_agent::CallableSymbol> callable =
            runtime_agent::callableSymbols(qtCore, listing);
        const bool callsMalloc =
            std::any_of(callable.begin(), callable.end(),
                        [](const runtime_agent::CallableSymbol& one) {
                            return one.symbol == "malloc";
                        });
        if (!callsMalloc && listing.empty()) {
            std::printf("  skip  this Qt does not call malloc through a slot\n");
            return 0;
        }
        std::printf("  FAIL QtCore calls malloc and the slot could not be resolved: %s\n",
                    error.c_str());
        return 1;
    }
    check(site.caller.name.find("libQt6Core.so") != std::string::npos,
          "the slot belongs to QtCore, named as the loader knows it");
    check(site.caller.known(), "and the resolve handed back an identity to quote later");

    // Built before the redirect, so encoding it afterwards is the only thing
    // being measured and nothing counts the setup.
    const QByteArray input(4096, 'x');
    const int before = replacementCalls;

    originalAllocator = reinterpret_cast<Allocator>(site.resolved);
    check(originalAllocator != nullptr, "the slot handed back something to chain to");

    runtime_agent::GotBinding bound;
    if (!registry.bind(site, reinterpret_cast<void*>(&countingMalloc), 1,
                       runtime_agent::LandingPadRule::Required, bound, error)) {
        std::printf("  FAIL could not bind: %s\n", error.c_str());
        return 1;
    }
    check(bound.original == site.resolved, "the binding names what the loader put there");

    // What this executable allocates goes through its own table, which nothing
    // touched, so it must still reach the real one. This is the limitation
    // stated as a measurement: the redirect belongs to one caller, and anyone
    // reporting it as replacing a function for the process would be wrong.
    //
    // The size is volatile and the allocation is written to, because a compiler
    // is entitled to delete an allocation whose result nobody uses, and this
    // would then pass without a call having happened.
    volatile std::size_t wanted = 64;
    auto* mine = static_cast<char*>(std::malloc(wanted));
    check(mine != nullptr, "this executable can still allocate");
    if (mine != nullptr) {
        mine[0] = 'a';
        check(mine[0] == 'a' && replacementCalls == before,
              "and its own allocation is not redirected");
        std::free(mine);
    }

    const QByteArray encoded = input.toBase64();
    check(encoded.size() > input.size(), "QtCore still does its work");
    const int inside = replacementCalls;
    check(inside > before, "and QtCore's allocation did reach the replacement");

    std::printf("what the registry says is in the slot\n");
    {
        runtime_agent::GotSlotStatus status;
        check(registry.statusOf(site.slot, status), "the slot is one the registry knows");
        check(status.selectedBinding == bound.id, "and it names this binding as selected");
        check(status.selectedOwner == 1, "under the owner that made it");
        check(status.original == site.resolved, "with the loader's value kept as original");
        check(status.liveBindings == 1, "and one binding live");
        check(!status.mappingLeftWritable,
              "and the page put back to the permissions the loader chose");
    }

    std::printf("a second binding on the same slot, released out of order\n");
    {
        // Resolving again now reads the replacement. The registry captured the
        // original on the first bind and does not look again, which is what
        // stops the replacement becoming the thing a restore puts back.
        runtime_agent::GotSite second;
        check(runtime_agent::resolveGotSlot(qtCore, "malloc", second, error),
              "the slot resolves again while it is redirected");
        check(second.resolved == reinterpret_cast<void*>(&countingMalloc),
              "and reads the replacement, which is why the original is captured once");

        runtime_agent::GotBinding stacked;
        const bool stackedBound =
            registry.bind(second, reinterpret_cast<void*>(&alsoCountingMalloc), 2,
                          runtime_agent::LandingPadRule::Required, stacked, error);
        check(stackedBound, stackedBound ? "a second owner can bind on top" : error.c_str());
        check(stacked.original == site.resolved,
              "and is still told the loader's value, not the first replacement");
        check(stacked.previous == reinterpret_cast<void*>(&countingMalloc),
              "and told what it displaced");

        const int firstBefore = replacementCalls;
        const int secondBefore = secondReplacementCalls;
        const QByteArray whileStacked = input.toBase64();
        check(!whileStacked.isEmpty(), "QtCore still works with two bound");
        check(secondReplacementCalls > secondBefore && replacementCalls == firstBefore,
              "and reaches the newest binding only");

        // Releasing the one underneath, which is not selected, must change
        // nothing about what the slot names.
        const bool buriedReleased = registry.unbind(bound.id, 1, error);
        check(buriedReleased,
              buriedReleased ? "the buried binding releases" : error.c_str());
        const int stillSecond = secondReplacementCalls;
        const QByteArray afterBuried = input.toBase64();
        check(!afterBuried.isEmpty(), "QtCore still works after that");
        check(secondReplacementCalls > stillSecond,
              "and the newest binding is still what it reaches");

        check(!registry.unbind(bound.id, 1, error) && !error.empty(),
              "releasing it twice is refused");
        check(!registry.unbind(stacked.id, 99, error)
                  && error.find("another owner") != std::string::npos,
              "and releasing somebody else's is refused by owner");

        // Releasing the last one puts back what the loader had.
        const bool lastReleased = registry.unbind(stacked.id, 2, error);
        check(lastReleased, lastReleased ? "the last binding releases" : error.c_str());
        const int quiet = secondReplacementCalls;
        const int alsoQuiet = replacementCalls;
        const QByteArray afterAll = input.toBase64();
        check(afterAll == encoded, "QtCore gives the same answer as before any of it");
        check(secondReplacementCalls == quiet && replacementCalls == alsoQuiet,
              "and reaches neither replacement");

        runtime_agent::GotSlotStatus status;
        check(registry.statusOf(site.slot, status), "the slot is still known");
        check(status.selectedBinding == 0 && status.liveBindings == 0,
              "with nothing selected and nothing live");
        check(*site.slot == site.resolved, "and holds exactly what the loader put there");
    }

    std::printf("a replacement an indirect branch may not land on\n");
    {
        runtime_agent::GotSite again;
        check(runtime_agent::resolveGotSlot(qtCore, "malloc", again, error),
              "the slot resolves once more");
        runtime_agent::GotBinding refused;
        check(!registry.bind(again, reinterpret_cast<void*>(&unmarkedMalloc), 3,
                             runtime_agent::LandingPadRule::Required, refused, error),
              "is refused where the marker is required");
        check(error.find("indirect branch") != std::string::npos,
              error.empty() ? "with a reason" : error.c_str());
        check(*again.slot == again.resolved, "and the slot is untouched");

        runtime_agent::GotBinding accepted;
        check(registry.bind(again, reinterpret_cast<void*>(&unmarkedMalloc), 3,
                            runtime_agent::LandingPadRule::NotChecked, accepted, error),
              "and accepted where the caller says the process is not checking");
        check(registry.unbind(accepted.id, 3, error), "then released");
    }

    std::printf("a caller built with no stubs, whose slots are in the other table\n");
    {
        const runtime_agent::CallerQuery noPlt =
            runtime_agent::CallerQuery::byName("libno_plt_caller.so");

        std::string listing;
        const std::vector<runtime_agent::CallableSymbol> callable =
            runtime_agent::callableSymbols(noPlt, listing);
        check(listing.empty(), "the object is loaded");

        const bool anyDirect =
            std::any_of(callable.begin(), callable.end(),
                        [](const runtime_agent::CallableSymbol& one) {
                            return one.kind == runtime_agent::CallKind::DirectLoad;
                        });
        check(anyDirect, "and reaches at least one function straight through a slot");

        runtime_agent::GotSite measure;
        if (runtime_agent::resolveGotSlot(noPlt, "strlen", measure, error)) {
            check(measure.kind == runtime_agent::CallKind::DirectLoad,
                  "its strlen is reached with no stub in between");

            const char* text = "twelve chars";
            check(noPltCallerMeasures(text) == std::strlen(text),
                  "it measures correctly before anything is bound");

            runtime_agent::GotBinding measured;
            const bool measureBound =
                registry.bind(measure, reinterpret_cast<void*>(&countingStrlen), 4,
                              runtime_agent::LandingPadRule::Required, measured, error);
            check(measureBound, measureBound ? "and the slot binds" : error.c_str());
            const int was = measureCalls;
            check(noPltCallerMeasures(text) == std::strlen(text),
                  "it still measures correctly through the replacement");
            check(measureCalls > was, "and the replacement is what it reached");
            check(std::strlen(text) == 12, "while this executable's own strlen is untouched");
            check(measureCalls == was + 1, "and did not count that one");

            check(registry.unbind(measured.id, 4, error), "then releases");
            const int done = measureCalls;
            check(noPltCallerMeasures(text) == std::strlen(text), "and measures as before");
            check(measureCalls == done, "reaching the replacement no longer");
        } else {
            std::printf("  FAIL a no-stub object's strlen slot was not found: %s\n",
                        error.c_str());
            ++failures;
        }
    }

    std::printf("a store that lands and a permission that does not come back\n");
    {
        // The slot names the replacement and the page is still writable. A
        // caller told only that the write failed would report the call as not
        // redirected while it is, so the two are told apart.
        //
        // Its own registry, because this one deliberately cannot put the
        // permission back and the shared one must not be left that way.
        runtime_agent::GotRegistry awkward(&failRestoreProtect);
        runtime_agent::GotSite site3;
        std::string protectError;
        if (runtime_agent::resolveGotSlot(qtCore, "malloc", site3, protectError)
            && (site3.pageProtection & PROT_WRITE) == 0) {
            runtime_agent::GotBinding partial;
            const bool bound3 =
                awkward.bind(site3, reinterpret_cast<void*>(&countingMalloc), 7,
                             runtime_agent::LandingPadRule::Required, partial, protectError);
            check(bound3, "the binding is reported as made, because the slot names it");
            check(*site3.slot == reinterpret_cast<void*>(&countingMalloc),
                  "and the slot really does name it");

            runtime_agent::GotSlotStatus awkwardStatus;
            check(awkward.statusOf(site3.slot, awkwardStatus), "the slot is known");
            check(awkwardStatus.mappingLeftWritable,
                  "and the page is recorded as left writable");

            int nowIs = 0;
            std::string readError;
            check(runtime_agent::pageProtectionOf(site3.slot, nowIs, readError)
                      && (nowIs & PROT_WRITE) != 0,
                  "which the kernel agrees with");

            // Put back with a working permission call, so nothing after this
            // runs with a writable table.
            check(runtime_agent::restoreGotSlot(site3).complete(),
                  "and a working restore puts both the value and the page back");
            check(runtime_agent::pageProtectionOf(site3.slot, nowIs, readError)
                      && (nowIs & PROT_WRITE) == 0,
                  "leaving the page as the loader had it");
        } else {
            std::printf("  skip  this slot's page is already writable, so there is no "
                        "permission to fail to put back\n");
        }
    }

    std::printf("a release that cannot be published leaves the binding live\n");
    {
        // The slot has to name the replacement for the release to need a store
        // at all, so it is bound with a working permission call and released
        // through a registry that cannot make the page writable.
        runtime_agent::GotSite site4;
        std::string why;
        if (runtime_agent::resolveGotSlot(qtCore, "malloc", site4, why)
            && (site4.pageProtection & PROT_WRITE) == 0) {
            runtime_agent::GotRegistry stubborn(&switchableProtect);
            runtime_agent::GotBinding held;
            check(stubborn.bind(site4, reinterpret_cast<void*>(&countingMalloc), 11,
                                runtime_agent::LandingPadRule::Required, held, why),
                  why.empty() ? "a binding is made through a working permission call"
                              : why.c_str());
            check(*site4.slot == reinterpret_cast<void*>(&countingMalloc),
                  "and the slot names it");

            refusingWrites.store(true, std::memory_order_release);
            why.clear();
            check(!stubborn.unbind(held.id, 11, why),
                  "releasing it is refused when the page cannot be made writable");
            check(*site4.slot == reinterpret_cast<void*>(&countingMalloc),
                  "and the slot still names the replacement, which is what runs");

            runtime_agent::GotSlotStatus after;
            check(stubborn.statusOf(site4.slot, after), "the slot is still known");
            check(after.selected == reinterpret_cast<void*>(&countingMalloc),
                  "and status names what the process actually reaches");
            check(after.selectedBinding == held.id,
                  "under the binding that still owns it");

            // The point of not committing the release: it can be asked for
            // again once the permission call works.
            refusingWrites.store(false, std::memory_order_release);
            why.clear();
            check(stubborn.unbind(held.id, 11, why),
                  why.empty() ? "and the same release succeeds once it can be published"
                              : why.c_str());
            check(*site4.slot == site4.resolved,
                  "putting the loader's own value back");
        } else {
            std::printf("  skip  this slot's page is already writable, so a store into it "
                        "cannot be made to fail\n");
        }
    }

    std::printf("naming a caller that could be more than one\n");
    {
        // Two objects are loaded whose names end in the same characters, and
        // both call malloc through a slot. Asking by that suffix has to say it
        // cannot choose, because taking the first would be a choice nobody
        // made and the caller could not see that there had been one.
        const runtime_agent::CallerQuery ambiguous =
            runtime_agent::CallerQuery::byName("no_plt_caller.so");

        std::vector<runtime_agent::GotSite> many;
        std::string ambiguousError;
        const bool listed =
            runtime_agent::resolveGotSlots(ambiguous, "malloc", many, ambiguousError);
        check(listed, listed ? "both objects' slots are found" : ambiguousError.c_str());
        check(many.size() == 2, "and there are two of them");

        runtime_agent::GotSite one;
        check(!runtime_agent::resolveGotSlot(ambiguous, "malloc", one, ambiguousError),
              "asking for one is refused where two match");
        check(ambiguousError.find("not something this") != std::string::npos,
              "with a reason naming how many there were");

        if (many.size() == 2) {
            check(!(many[0].caller == many[1].caller),
                  "the two are told apart by where they are loaded, not by name");

            // Naming one exactly gets that one, which is what an identity from
            // an earlier resolve is for.
            const runtime_agent::CallerQuery exact =
                runtime_agent::CallerQuery::exactly(many.front().caller);
            runtime_agent::GotSite named;
            const bool namedOk =
                runtime_agent::resolveGotSlot(exact, "malloc", named, ambiguousError);
            check(namedOk, namedOk ? "naming one exactly resolves" : ambiguousError.c_str());
            check(named.caller == many.front().caller, "and is the one that was named");
            check(named.slot == many.front().slot, "with the slot that belongs to it");
        }
    }

    std::printf("%s\n", failures == 0 ? "the redirect reached unrebuilt library code and is "
                                        "owned"
                                      : "it did not");
    return failures == 0 ? 0 : 1;
}
