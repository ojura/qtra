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

    // What the loader put there, kept so a caller can chain to it and so a
    // release can put it back.
    void* resolved = nullptr;

    [[nodiscard]] bool valid() const noexcept { return slot != nullptr; }
};

// Finds the slot a named symbol is called through, from a named object.
//
// callerObject is matched against the loader's name for each object, by suffix,
// so "libQt6Core.so" finds a versioned file name. Empty means the main
// executable.
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
