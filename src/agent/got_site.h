#pragma once

// Where a call into another shared object can be redirected, without touching
// the code of either.
//
// A call from one object to a function in another goes through that object's
// procedure linkage table, which loads its destination from a slot in the
// global offset table. Replacing what a caller reaches is then a store into
// that slot, and the callee's own bytes are never involved.
//
// This is the case a prepared entry cannot serve. Reserving space at a
// function's entry means building the object that defines it, and Qt's
// libraries are not ours to build. The relocations are, because they belong to
// the object doing the calling.
//
// What it redirects and what it does not: every call made through the resolved
// object's own table, which is every call from that object's code to that
// symbol. Calls made inside the defining library, or from a third object with
// its own table, keep reaching the original. So this replaces a function for a
// caller and not for a process, which is a narrower claim than replacing an
// entry and worth stating wherever it is offered.
//
// One store, aligned, into a slot the loader has already resolved. A thread
// reading it gets the old address or the new one, so nothing has to be stopped.
// The slot is normally mapped read-only after the loader finishes, so making it
// writable is a permission change and not a race.

#include <cstdint>
#include <string>
#include <vector>

namespace runtime_agent {

// One resolved slot, valid while the object holding it stays loaded.
struct GotSite {
    // The object whose calls this redirects, by the name the loader knows.
    // Empty for the main executable.
    std::string caller;

    // The symbol being called, as the relocation names it, version and all.
    std::string symbol;

    // The slot itself, which is what a store goes into.
    void** slot = nullptr;

    // What the loader's mapping said about the page holding the slot, so
    // restoring puts back the permissions it chose. An object bound lazily
    // keeps its table writable on purpose, and handing it back read-only would
    // fault the next symbol it resolved.
    int pageProtection = 0;

    // What was in the slot when it was read, kept so a release can put exactly
    // that back.
    //
    // Not promised to be the function. An object the loader bound lazily has a
    // stub here that resolves the symbol on first call and then replaces
    // itself, so this can be that stub. Calling it does reach the function on
    // the form seen here, but that is a property of one procedure linkage table
    // layout and not something this establishes, so a replacement wanting to
    // chain to the original should not assume it.
    void* resolved = nullptr;

    [[nodiscard]] bool valid() const noexcept { return slot != nullptr; }
};

// Finds the slot a named symbol is called through, from a named object.
//
// callerObject has to be a suffix of the loader's own name for the object, so
// "libQt6Core.so.6" matches "/lib/x86_64-linux-gnu/libQt6Core.so.6" and
// "libQt6Core.so" matches nothing, because the loaded name ends in the version.
// Give the soname as the loader knows it. Empty means the main executable.
//
// The result carries the name the loader used and the symbol as the relocation
// spells it, so a caller sees what was actually redirected instead of what it
// asked for.
//
// symbol is matched against the dynamic string table, which holds a bare name:
// the "@GLIBC_2.14" a disassembler shows comes from separate version tables
// this does not read. So two versions of one symbol cannot be told apart here,
// and a caller wanting a particular one has to look elsewhere. Where an object
// calls two versions of the same name, this finds the first relocation naming
// it, which is not a choice anyone made.
//
// symbol matches the relocation's symbol name, either exactly or ignoring the
// "@version" suffix, so a caller can name a function without knowing which
// version tag the library assigned it.
[[nodiscard]] bool resolveGotSlot(const std::string& callerObject,
                                  const std::string& symbol,
                                  GotSite& site,
                                  std::string& error);

// Every symbol the named object calls through its table.
//
// This is the answer to "what can I reach here", and it needs no debug
// information and no file access: the relocations are in the loaded image
// because the loader needed them.
[[nodiscard]] std::vector<std::string> callableSymbols(const std::string& callerObject,
                                                       std::string& error);

// What the kernel reports about the mapping holding an address, as the
// PROT_ flags. Offered so a caller, or a test, can check that a redirect and a
// restore left the mapping as the loader had it.
[[nodiscard]] bool pageProtectionOf(const void* address, int& protection, std::string& error);

// Points the caller's slot at something else.
//
// One aligned store, so a call in flight reads the old destination or the new
// one and nothing has to be stopped. The loader normally maps this read-only
// once it has finished resolving, so the store is wrapped in a permission
// change; that window is about this thread's own write and not about what other
// threads are executing.
//
// Nothing here checks that the replacement is a sensible thing to call. The
// slot is typed by the symbol's declaration, which this cannot see, so a
// replacement with the wrong signature is a crash the caller has asked for.
[[nodiscard]] bool redirectGotSlot(const GotSite& site, void* replacement, std::string& error);

// Puts back what the loader resolved, from what the site recorded.
[[nodiscard]] bool restoreGotSlot(const GotSite& site, std::string& error);

} // namespace runtime_agent
