// Reaching a function in a library this build does not produce.
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

#include "agent/got_site.h"

#include <QByteArray>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

// Forwards, because the point is which allocator Qt reaches and not what it
// does. Calling the real one by name is safe here: this executable's own calls
// go through its own table, and it is Qt's that has been redirected.
extern "C" void* countingMalloc(const std::size_t bytes)
{
    ++replacementCalls;
    return std::malloc(bytes);
}

} // namespace

int main()
{
    std::printf("a call Qt makes, redirected without rebuilding Qt\n");

    // Warmed before anything is resolved. A slot the loader has not bound yet
    // holds its resolver stub, and resolving refuses that, so asking first
    // would skip this test for a reason that has nothing to do with whether the
    // mechanism works.
    QByteArray warm = QByteArray("warm the allocator").toBase64();
    check(!warm.isEmpty(), "QtCore works before anything is touched");

    std::string error;
    runtime_agent::GotSite site;
    if (!runtime_agent::resolveGotSlot("libQt6Core.so.6", "malloc", site, error)) {
        // Skipping is only right where this Qt does not call malloc through its
        // table at all. Any other failure is this failing, and treating them
        // alike is how a test reports success for never having run.
        std::string listing;
        const std::vector<std::string> callable =
            runtime_agent::callableSymbols("libQt6Core.so.6", listing);
        const bool callsMalloc =
            std::find(callable.begin(), callable.end(), "malloc") != callable.end();
        if (!callsMalloc && listing.empty()) {
            std::printf("  skip  this Qt does not call malloc through its table\n");
            return 0;
        }
        std::printf("  FAIL QtCore calls malloc and the slot could not be resolved: %s\n",
                    error.c_str());
        return 1;
    }
    check(site.caller.find("libQt6Core.so") != std::string::npos,
          "the slot belongs to QtCore, named as the loader knows it");

    // Built before the redirect, so encoding it afterwards is the only thing
    // being measured and nothing counts the setup.
    const QByteArray input(4096, 'x');
    const int before = replacementCalls;

    if (!runtime_agent::redirectGotSlot(site, reinterpret_cast<void*>(&countingMalloc),
                                        error)) {
        std::printf("  FAIL could not redirect: %s\n", error.c_str());
        return 1;
    }

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

    if (!runtime_agent::restoreGotSlot(site, error)) {
        std::printf("  FAIL could not restore: %s\n", error.c_str());
        return 1;
    }

    const QByteArray again = input.toBase64();
    check(again == encoded, "QtCore gives the same answer afterwards");
    check(replacementCalls == inside, "and no longer reaches the replacement");

    std::printf("%s\n", failures == 0 ? "the redirect reached unrebuilt library code"
                                      : "it did not");
    return failures == 0 ? 0 : 1;
}
