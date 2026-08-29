#pragma once

// Where a call into another shared object can be redirected, without touching
// the code of either.
//
// A call from one object to a function in another loads its destination from a
// slot in that object's global offset table. Replacing what a caller reaches is
// then a store into that slot, and the callee's own bytes are never involved.
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

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace runtime_agent {

// How a caller reaches the symbol, which decides which table its slot is in.
enum class CallKind {
    // Through the procedure linkage table: a call lands on a stub, which jumps
    // through the slot. The relocation is a jump slot, in the table the loader
    // keeps for exactly these.
    ProcedureLinkageTable,

    // Straight through the slot, with no stub in between, which is what
    // -fno-plt produces. The relocation sits with the object's ordinary data
    // relocations and is filled in before anything runs, so there is never a
    // resolver stub in it.
    DirectLoad,
};

[[nodiscard]] const char* describe(CallKind kind) noexcept;

// Which loaded object a slot belongs to, in a form a later call can quote.
//
// A name alone does not identify one. Two objects can end with the same
// characters, and a caller naming a suffix has no way to say which it meant. The
// load address is what the loader itself uses to tell them apart, so a resolve
// hands it back and a later call gives it instead of the name.
struct CallerIdentity {
    // The loader's own name for the object. Empty for the main executable.
    std::string name;

    // Where it is loaded, which no two loaded objects share.
    const void* base = nullptr;

    [[nodiscard]] bool known() const noexcept { return base != nullptr; }

    [[nodiscard]] bool operator==(const CallerIdentity& other) const noexcept
    {
        return base == other.base;
    }
};

// One resolved slot, valid while the object holding it stays loaded.
struct GotSite {
    CallerIdentity caller;

    // The symbol being called, as the dynamic string table spells it, which is
    // a bare name with no version.
    std::string symbol;

    CallKind kind = CallKind::ProcedureLinkageTable;

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
    // The function, because a slot still holding a resolver stub is refused.
    // See the note on resolveGotSlots for why that is a refusal and not a
    // caveat.
    void* resolved = nullptr;

    [[nodiscard]] bool valid() const noexcept { return slot != nullptr; }
};

// What a store into a slot did, which is a different question from whether it
// succeeded.
//
// Writing runs as three steps: make the page writable where it is not, store,
// put the permission back. A failure in the third step leaves the slot holding
// the new destination, so a caller told only "false" would report the call as
// not redirected while it is.
enum class SlotWriteOutcome {
    NotWritten,
    Written,
    WrittenProtectionNotRestored,
};

struct SlotWriteResult {
    SlotWriteOutcome outcome = SlotWriteOutcome::NotWritten;
    std::string error;

    [[nodiscard]] bool complete() const noexcept
    {
        return outcome == SlotWriteOutcome::Written;
    }

    // Whether the slot may now name something other than it did. Anything
    // reporting this to a caller has to say yes here even when it also has an
    // error to give.
    [[nodiscard]] bool changedSlot() const noexcept
    {
        return outcome != SlotWriteOutcome::NotWritten;
    }
};

// Which objects to look in.
//
// An empty name means the main executable, which the loader knows by an empty
// name too. A non-empty one has to be a suffix of the loader's own name, so
// "libQt6Core.so.6" matches "/lib/x86_64-linux-gnu/libQt6Core.so.6" and
// "libQt6Core.so" matches nothing, because the loaded name ends in the version.
//
// Give an identity from an earlier resolve to name exactly one object and skip
// the question of what a suffix matches.
struct CallerQuery {
    std::string nameSuffix;
    CallerIdentity identity;

    [[nodiscard]] static CallerQuery byName(std::string suffix)
    {
        return CallerQuery{std::move(suffix), CallerIdentity{}};
    }

    [[nodiscard]] static CallerQuery exactly(CallerIdentity who)
    {
        return CallerQuery{std::string{}, std::move(who)};
    }
};

// Every slot matching the query, in the order the loader lists the objects.
//
// All of them, because a suffix can match two loaded objects and a symbol can
// have more than one relocation in one object. Returning the first would be a
// choice nobody made, and the caller cannot even see that there was one.
//
// A slot the loader has not finished with is refused, and that refusal is
// reported for the whole call. Until the symbol is bound, the slot holds a stub
// that resolves it on first call and then writes the answer into the slot,
// which makes the loader a second writer to the same word. A replacement stored
// there is correct until some thread takes that path, and is then overwritten
// by the resolver. The store being atomic keeps the word from tearing and does
// nothing about this. A caller wanting to redirect such an object has to get it
// bound first, which is what RTLD_NOW does at load time.
//
// symbol is matched against the dynamic string table, which holds a bare name:
// the "@GLIBC_2.14" a disassembler shows comes from separate version tables
// this does not read. So two versions of one symbol cannot be told apart here.
// Where an object calls two, both are returned and the caller can see that it
// has to choose.
[[nodiscard]] bool resolveGotSlots(const CallerQuery& caller,
                                   const std::string& symbol,
                                   std::vector<GotSite>& sites,
                                   std::string& error);

// The one slot matching, or a refusal naming how many there were.
//
// For a caller that expects one and would rather be told than guessed for.
[[nodiscard]] bool resolveGotSlot(const CallerQuery& caller,
                                  const std::string& symbol,
                                  GotSite& site,
                                  std::string& error);

// Every symbol the matching objects call through a slot, with how each is
// reached.
struct CallableSymbol {
    CallerIdentity caller;
    std::string symbol;
    CallKind kind = CallKind::ProcedureLinkageTable;
};

[[nodiscard]] std::vector<CallableSymbol> callableSymbols(const CallerQuery& caller,
                                                          std::string& error);

// What the kernel reports about the mapping holding an address, as the PROT_
// flags. Offered so a caller, or a test, can check that a redirect and a
// restore left the mapping as the loader had it.
[[nodiscard]] bool pageProtectionOf(const void* address, int& protection, std::string& error);

// Whether an address begins with the marker an indirect branch is allowed to
// land on where the processor is checking.
//
// Every way of reaching a slot's destination is an indirect branch: through the
// table it is a jump through the slot, and without a stub it is a call through
// it. So a destination with no marker faults where branch tracking is enforced,
// and works where it is not. Which of those a process is doing is decided when
// it starts, and cannot be read back portably from inside it.
//
// So this is checked and refused by default, and a caller that knows its
// process is not checking can say so. Refusing is the way round that fails
// where the replacement is wrong instead of where the process is strict, and
// the flag on a replacement's own build is what fixes it.
[[nodiscard]] bool hasLandingPad(const void* destination) noexcept;

// The permission call, as a parameter so a test can fail the one after the
// store.
//
// A real mprotect will not fail on demand, and that is the path worth reaching:
// it is the only one that leaves the slot naming the replacement while the
// caller is told the write did not finish. Anything reporting to a caller has
// to tell those two apart, and a path nothing can reach is a path nothing has
// checked.
using SlotProtectFunction = int (*)(void* address, std::size_t length, int protection);

// Points the caller's slot at something else, reporting what the store did.
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
[[nodiscard]] SlotWriteResult redirectGotSlot(const GotSite& site,
                                              void* replacement,
                                              SlotProtectFunction protect = nullptr);

// Puts back exactly the value the slot held when it was resolved, which is the
// function: a slot still holding a stub is refused at resolve time.
[[nodiscard]] SlotWriteResult restoreGotSlot(const GotSite& site,
                                             SlotProtectFunction protect = nullptr);

} // namespace runtime_agent
