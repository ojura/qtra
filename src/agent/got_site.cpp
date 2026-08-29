#include "agent/got_site.h"

#include "agent/errno_text.h"
#include "agent/page_span.h"
#include "agent/patch_site.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

namespace runtime_agent {
namespace {

// One object's relocations, from its own dynamic section.
//
// Two tables, because a caller reaches a function in two ways. The jump slots
// are what a call through the procedure linkage table uses. The ordinary data
// relocations hold the rest, and among them are the slots a call reaches
// straight through when the object was built without stubs.
struct DynamicTables {
    const ElfW(Rela)* jumpSlots = nullptr;
    std::size_t jumpSlotBytes = 0;
    const ElfW(Rela)* data = nullptr;
    std::size_t dataBytes = 0;
    std::size_t dataEntryBytes = sizeof(ElfW(Rela));
    const ElfW(Sym)* symbols = nullptr;
    const char* strings = nullptr;

    [[nodiscard]] bool usable() const noexcept
    {
        return symbols != nullptr && strings != nullptr;
    }
};

// A dynamic entry's pointer, as an address in this process.
//
// The loader leaves these already relocated for most objects, and link-time
// values for some. Both happen in practice, so the base is added only where the
// value is plainly not an address yet. An object's own load address is the
// lowest thing it can contain, so anything below it has not been adjusted.
const void* asLoadedAddress(const ElfW(Addr) value, const ElfW(Addr) base)
{
    const ElfW(Addr) resolved = value < base ? value + base : value;
    return reinterpret_cast<const void*>(resolved);
}

DynamicTables tablesOf(const dl_phdr_info& object)
{
    DynamicTables tables;
    const ElfW(Dyn)* dynamic = nullptr;
    for (int i = 0; i < object.dlpi_phnum; ++i) {
        if (object.dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dynamic = reinterpret_cast<const ElfW(Dyn)*>(object.dlpi_addr
                                                         + object.dlpi_phdr[i].p_vaddr);
            break;
        }
    }
    if (dynamic == nullptr) {
        return tables;
    }

    bool jumpSlotsAreRela = false;
    for (const ElfW(Dyn)* entry = dynamic; entry->d_tag != DT_NULL; ++entry) {
        switch (entry->d_tag) {
        case DT_JMPREL:
            tables.jumpSlots = static_cast<const ElfW(Rela)*>(
                asLoadedAddress(entry->d_un.d_ptr, object.dlpi_addr));
            break;
        case DT_PLTRELSZ:
            tables.jumpSlotBytes = entry->d_un.d_val;
            break;
        case DT_PLTREL:
            jumpSlotsAreRela = entry->d_un.d_val == DT_RELA;
            break;
        case DT_RELA:
            tables.data = static_cast<const ElfW(Rela)*>(
                asLoadedAddress(entry->d_un.d_ptr, object.dlpi_addr));
            break;
        case DT_RELASZ:
            tables.dataBytes = entry->d_un.d_val;
            break;
        case DT_RELAENT:
            tables.dataEntryBytes = entry->d_un.d_val;
            break;
        case DT_SYMTAB:
            tables.symbols = static_cast<const ElfW(Sym)*>(
                asLoadedAddress(entry->d_un.d_ptr, object.dlpi_addr));
            break;
        case DT_STRTAB:
            tables.strings = static_cast<const char*>(
                asLoadedAddress(entry->d_un.d_ptr, object.dlpi_addr));
            break;
        default:
            break;
        }
    }

    if (!jumpSlotsAreRela) {
        // Every x86-64 object uses addend-carrying relocations. One that does
        // not is a shape this has not been written for, and guessing at it
        // would be reading a different struct through the same pointer.
        tables.jumpSlots = nullptr;
        tables.jumpSlotBytes = 0;
    }
    if (tables.dataEntryBytes != sizeof(ElfW(Rela))) {
        tables.data = nullptr;
        tables.dataBytes = 0;
    }
    return tables;
}

// Where an object's code is, which is what a resolver stub sits in.
struct Span {
    ElfW(Addr) lowest = ~ElfW(Addr){0};
    ElfW(Addr) highest = 0;

    [[nodiscard]] bool contains(const void* address) const noexcept
    {
        const auto value = reinterpret_cast<ElfW(Addr)>(address);
        return value >= lowest && value < highest;
    }
};

Span executableSpanOf(const dl_phdr_info& object)
{
    Span span;
    for (int i = 0; i < object.dlpi_phnum; ++i) {
        const ElfW(Phdr)& header = object.dlpi_phdr[i];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_X) == 0) {
            continue;
        }
        const ElfW(Addr) from = object.dlpi_addr + header.p_vaddr;
        span.lowest = from < span.lowest ? from : span.lowest;
        const ElfW(Addr) to = from + header.p_memsz;
        span.highest = to > span.highest ? to : span.highest;
    }
    return span;
}

bool namesObject(const dl_phdr_info& object, const CallerQuery& query)
{
    if (query.identity.known()) {
        return reinterpret_cast<const void*>(object.dlpi_addr) == query.identity.base;
    }
    const std::string name = object.dlpi_name != nullptr ? object.dlpi_name : "";
    if (query.nameSuffix.empty()) {
        // The loader calls the main executable by an empty name.
        return name.empty();
    }
    return name.size() >= query.nameSuffix.size()
        && name.compare(name.size() - query.nameSuffix.size(), query.nameSuffix.size(),
                        query.nameSuffix)
            == 0;
}

CallerIdentity identityOf(const dl_phdr_info& object)
{
    CallerIdentity who;
    who.name = object.dlpi_name != nullptr ? object.dlpi_name : "";
    who.base = reinterpret_cast<const void*>(object.dlpi_addr);
    return who;
}

struct Search {
    const CallerQuery* caller = nullptr;
    const std::string* symbol = nullptr;
    std::vector<GotSite>* sites = nullptr;
    std::vector<CallableSymbol>* names = nullptr;
    bool sawObject = false;
    bool sawUnbound = false;
    std::string failure;
};

// One relocation, offered to whatever the search is collecting.
void consider(const dl_phdr_info& object,
              const DynamicTables& tables,
              const Span& code,
              const ElfW(Rela)& relocation,
              const CallKind kind,
              Search& search)
{
    const ElfW(Sym)& symbol = tables.symbols[ELF64_R_SYM(relocation.r_info)];
    const char* name = tables.strings + symbol.st_name;
    if (name == nullptr || name[0] == '\0') {
        return;
    }

    if (search.names != nullptr) {
        search.names->push_back(CallableSymbol{identityOf(object), name, kind});
        return;
    }
    if (*search.symbol != name) {
        return;
    }

    auto** slot = reinterpret_cast<void**>(object.dlpi_addr + relocation.r_offset);

    GotSite site;
    site.caller = identityOf(object);
    site.symbol = name;
    site.kind = kind;
    site.slot = slot;
    site.resolved = *slot;

    // Recorded at resolve time, so a restore puts back what the loader chose.
    // Failing to read it fails the resolve: defaulting to read-only is how the
    // permissions get downgraded, which is the thing this field exists to
    // prevent, and a site that cannot be restored correctly is not one anybody
    // should be handed.
    if (!pageProtectionOf(slot, site.pageProtection, search.failure)) {
        return;
    }

    // A jump slot still pointing into its own object's code holds that object's
    // resolver stub, so the loader has not finished with it and will write the
    // real answer there on first call. A slot reached without a stub is filled
    // before anything runs, so there is nothing of the sort to find.
    //
    // An object that resolves a symbol to itself would look the same. That is a
    // refusal where a redirect would have worked, which is the direction to be
    // wrong in: the caller is told what was found and can say what it meant.
    if (kind == CallKind::ProcedureLinkageTable && code.contains(site.resolved)) {
        search.sawUnbound = true;
        return;
    }

    search.sites->push_back(std::move(site));
}

int visit(dl_phdr_info* object, std::size_t, void* opaque)
{
    auto& search = *static_cast<Search*>(opaque);
    if (!namesObject(*object, *search.caller)) {
        return 0;
    }
    search.sawObject = true;

    const DynamicTables tables = tablesOf(*object);
    if (!tables.usable()) {
        return 0;
    }
    const Span code = executableSpanOf(*object);

    const std::size_t jumpSlots = tables.jumpSlots != nullptr
        ? tables.jumpSlotBytes / sizeof(ElfW(Rela))
        : 0;
    for (std::size_t i = 0; i < jumpSlots; ++i) {
        const ElfW(Rela)& relocation = tables.jumpSlots[i];
        if (ELF64_R_TYPE(relocation.r_info) != R_X86_64_JUMP_SLOT) {
            continue;
        }
        consider(*object, tables, code, relocation, CallKind::ProcedureLinkageTable, search);
    }

    // The same object's ordinary data relocations, where a build without stubs
    // puts the slots its calls go through. Only the ones naming a function:
    // this table also holds slots for data, and storing a function address into
    // one of those would be redirecting something that is not a call.
    const std::size_t dataEntries = tables.data != nullptr
        ? tables.dataBytes / sizeof(ElfW(Rela))
        : 0;
    for (std::size_t i = 0; i < dataEntries; ++i) {
        const ElfW(Rela)& relocation = tables.data[i];
        if (ELF64_R_TYPE(relocation.r_info) != R_X86_64_GLOB_DAT) {
            continue;
        }
        const ElfW(Sym)& symbol = tables.symbols[ELF64_R_SYM(relocation.r_info)];
        if (ELF64_ST_TYPE(symbol.st_info) != STT_FUNC
            && ELF64_ST_TYPE(symbol.st_info) != STT_GNU_IFUNC) {
            continue;
        }
        consider(*object, tables, code, relocation, CallKind::DirectLoad, search);
    }
    return 0;
}

// Writes one pointer, leaving the mapping as the loader had it.
//
// Making it writable is skipped where it already is, which is the common case
// for a lazily bound object and the case where forcing it back to read-only
// would break the next lazy resolution on that page.
SlotWriteResult storeIntoSlot(void** slot, void* value, const int protection,
                              SlotProtectFunction protect)
{
    if (protect == nullptr) {
        protect = &::mprotect;
    }
    SlotWriteResult result;
    const std::size_t bytes = pageSize();
    const auto address = reinterpret_cast<std::uintptr_t>(slot);
    std::uintptr_t base = 0;
    std::size_t span = 0;
    // A slot is one aligned pointer, so this is the single page it sits in.
    // Asked for rather than assumed, because the same call reports a page size
    // that could not be determined, which the mask would otherwise hide.
    if (!pageSpan(address, sizeof(void*), bytes, base, span)) {
        result.error = "the system page size could not be determined, so the pages holding "
                       "the slot cannot be named";
        return result;
    }
    auto* page = reinterpret_cast<void*>(base);
    const bool alreadyWritable = (protection & PROT_WRITE) != 0;

    if (!alreadyWritable && protect(page, span, protection | PROT_WRITE) != 0) {
        result.error = std::string("could not make the slot writable: ")
            + errnoText(errno);
        return result;
    }

    // One aligned pointer word, stored atomically, so a caller loading it gets
    // the old destination or the new one and never a mixture.
    static_assert(std::atomic_ref<void*>::is_always_lock_free);
    std::atomic_ref<void*>(*slot).store(value, std::memory_order_release);
    result.outcome = SlotWriteOutcome::Written;

    if (!alreadyWritable && protect(page, span, protection) != 0) {
        result.outcome = SlotWriteOutcome::WrittenProtectionNotRestored;
        result.error = std::string("the slot now names the new destination and the mapping "
                                   "could not be put back to the permissions the loader "
                                   "chose: ")
            + errnoText(errno);
    }
    return result;
}

} // namespace

const char* describe(const CallKind kind) noexcept
{
    switch (kind) {
    case CallKind::ProcedureLinkageTable:
        return "through the procedure linkage table";
    case CallKind::DirectLoad:
        return "straight through the slot";
    }
    return "unknown";
}

// What the kernel says about the mapping holding an address.
//
// Read, not assumed. An object the loader bound eagerly has its table mapped
// read-only when it finishes; one bound lazily keeps it writable because the
// loader still has symbols to resolve into it. Handing the second kind back as
// read-only would fault the next call it tried to resolve.
bool pageProtectionOf(const void* address, int& protection, std::string& error)
{
    std::FILE* maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr) {
        error = std::string("could not read this process's mappings: ") + errnoText(errno);
        return false;
    }
    const auto wanted = reinterpret_cast<std::uintptr_t>(address);
    char line[512];
    bool found = false;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        std::uintptr_t from = 0;
        std::uintptr_t to = 0;
        char flags[5] = {};
        if (std::sscanf(line, "%lx-%lx %4s", &from, &to, flags) != 3) {
            continue;
        }
        if (wanted < from || wanted >= to) {
            continue;
        }
        protection = 0;
        if (flags[0] == 'r') { protection |= PROT_READ; }
        if (flags[1] == 'w') { protection |= PROT_WRITE; }
        if (flags[2] == 'x') { protection |= PROT_EXEC; }
        found = true;
        break;
    }
    (void)std::fclose(maps);
    if (!found) {
        error = "the slot is not in any mapping this process reports";
    }
    return found;
}

bool hasLandingPad(const void* destination) noexcept
{
    if (destination == nullptr) {
        return false;
    }
    // endbr64, which is what an indirect branch is permitted to land on.
    const auto* bytes = static_cast<const std::uint8_t*>(destination);
    return std::memcmp(bytes, endbr64Bytes, sizeof(endbr64Bytes)) == 0;
}

bool resolveGotSlots(const CallerQuery& caller,
                     const std::string& symbol,
                     std::vector<GotSite>& sites,
                     std::string& error)
{
    sites.clear();
    error.clear();
    if (symbol.empty()) {
        error = "a symbol name is required";
        return false;
    }

    Search search;
    search.caller = &caller;
    search.symbol = &symbol;
    search.sites = &sites;
    ::dl_iterate_phdr(&visit, &search);

    if (!search.sawObject) {
        error = caller.identity.known()
            ? "no object is loaded at that address any more"
            : "no loaded object named '" + caller.nameSuffix
                + "'; the name has to be a suffix of the loader's own, which ends in the "
                  "version, so give the soname as the loader knows it";
        return false;
    }
    if (!search.failure.empty()) {
        error = "the slot for '" + symbol + "' was found and what the loader had done to its "
                "page could not be read, so restoring it later could not put that back: "
            + search.failure;
        sites.clear();
        return false;
    }
    if (sites.empty() && search.sawUnbound) {
        error = "'" + symbol + "' is not bound yet in that object, so its slot holds the "
                "loader's own stub. Storing a replacement there works until some thread "
                "takes that path, and the resolver then writes the real function over it. "
                "Load the object with its symbols bound before redirecting it";
        return false;
    }
    if (sites.empty()) {
        error = "'"
            + (caller.identity.known()
                   ? (caller.identity.name.empty() ? std::string("the main executable")
                                                   : caller.identity.name)
                   : (caller.nameSuffix.empty() ? std::string("the main executable")
                                                : caller.nameSuffix))
            + "' does not call '" + symbol
            + "' through a slot. Either it never calls it, or the call is inside the object "
              "that defines it, where there is no relocation to redirect";
        return false;
    }
    return true;
}

bool resolveGotSlot(const CallerQuery& caller,
                    const std::string& symbol,
                    GotSite& site,
                    std::string& error)
{
    site = GotSite{};
    std::vector<GotSite> found;
    if (!resolveGotSlots(caller, symbol, found, error)) {
        return false;
    }
    if (found.size() != 1) {
        error = "'" + symbol + "' is reached through " + std::to_string(found.size())
            + " slots matching that caller, so which one was meant is not something this "
              "can decide. Ask for all of them, or name one object exactly";
        return false;
    }
    site = found.front();
    return true;
}

std::vector<CallableSymbol> callableSymbols(const CallerQuery& caller, std::string& error)
{
    std::vector<CallableSymbol> names;
    error.clear();
    Search search;
    search.caller = &caller;
    search.names = &names;
    ::dl_iterate_phdr(&visit, &search);
    if (!search.sawObject) {
        error = "no loaded object matched";
    }
    return names;
}

SlotWriteResult redirectGotSlot(const GotSite& site, void* const replacement,
                                const SlotProtectFunction protect)
{
    SlotWriteResult result;
    if (!site.valid()) {
        result.error = "the site was never resolved";
        return result;
    }
    if (replacement == nullptr) {
        result.error = "a replacement is required";
        return result;
    }
    return storeIntoSlot(site.slot, replacement, site.pageProtection, protect);
}

SlotWriteResult restoreGotSlot(const GotSite& site, const SlotProtectFunction protect)
{
    SlotWriteResult result;
    if (!site.valid()) {
        result.error = "the site was never resolved";
        return result;
    }
    return storeIntoSlot(site.slot, site.resolved, site.pageProtection, protect);
}

} // namespace runtime_agent
