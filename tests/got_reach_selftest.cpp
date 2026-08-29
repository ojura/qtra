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

#include <cstdio>
#include <cstdlib>

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

    std::string error;
    runtime_agent::GotSite site;
    if (!runtime_agent::resolveGotSlot("libQt6Core.so.6", "malloc", site, error)) {
        // A Qt built to allocate through something else, or a different soname,
        // is not a failure of the mechanism.
        std::printf("  skip  this Qt does not call malloc through its table: %s\n",
                    error.c_str());
        return 0;
    }
    check(site.caller.find("libQt6Core.so") != std::string::npos,
          "the slot belongs to QtCore, named as the loader knows it");

    // Warmed first, so anything allocated once on the way in is already done.
    QByteArray warm = QByteArray("warm the allocator").toBase64();
    (void)warm;
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
    void* mine = std::malloc(64);
    check(replacementCalls == before,
          "this executable's own allocation is not redirected");
    std::free(mine);

    QByteArray during = QByteArray("bytes for QtCore to encode").toBase64();
    check(!during.isEmpty(), "QtCore still does its work");
    const int inside = replacementCalls;
    check(inside > before, "and QtCore's allocation did reach the replacement");

    if (!runtime_agent::restoreGotSlot(site, error)) {
        std::printf("  FAIL could not restore: %s\n", error.c_str());
        return 1;
    }

    QByteArray after = QByteArray("and more once it is back").toBase64();
    check(!after.isEmpty(), "QtCore still works afterwards");
    check(replacementCalls == inside, "and no longer reaches the replacement");

    std::printf("%s\n", failures == 0 ? "the redirect reached unrebuilt library code"
                                      : "it did not");
    return failures == 0 ? 0 : 1;
}
