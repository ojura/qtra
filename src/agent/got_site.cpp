#include "agent/got_site.h"

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

// What one loaded object's dynamic section says about its own relocations.
struct DynamicTables {
    const ElfW(Rela)* relocations = nullptr;
    std::size_t relocationBytes = 0;
    const ElfW(Sym)* symbols = nullptr;
    const char* strings = nullptr;

    [[nodiscard]] bool complete() const noexcept
    {
        return relocations != nullptr && symbols != nullptr && strings != nullptr
            && relocationBytes >= sizeof(ElfW(Rela));
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

    // Only the table used by the procedure linkage table. DT_RELA covers other
    // relocations, and a store into one of those would be redirecting something
    // that is not a call through the table.
    bool pltIsRela = false;
    for (const ElfW(Dyn)* entry = dynamic; entry->d_tag != DT_NULL; ++entry) {
        switch (entry->d_tag) {
        case DT_JMPREL:
            tables.relocations = static_cast<const ElfW(Rela)*>(
                asLoadedAddress(entry->d_un.d_ptr, object.dlpi_addr));
            break;
        case DT_PLTRELSZ:
            tables.relocationBytes = entry->d_un.d_val;
            break;
        case DT_PLTREL:
            pltIsRela = entry->d_un.d_val == DT_RELA;
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

    if (!pltIsRela) {
        // Every x86-64 object uses addend-carrying relocations. One that does
        // not is a shape this has not been written for, and guessing at it
        // would be reading a different struct through the same pointer.
        return DynamicTables{};
    }
    return tables;
}

bool namesObject(const char* loaderName, const std::string& wanted)
{
    const std::string name = loaderName != nullptr ? loaderName : "";
    if (wanted.empty()) {
        // The loader calls the main executable by an empty name.
        return name.empty();
    }
    return name.size() >= wanted.size()
        && name.compare(name.size() - wanted.size(), wanted.size(), wanted) == 0;
}

// Whether a relocation's symbol is the one asked for.
//
// Compared whole. The dynamic string table holds a bare name, so there is no
// version to strip: what a disassembler prints as "memcpy@GLIBC_2.14" is the
// bare name plus a tag it read from tables this does not.
bool namesSymbol(const char* relocationSymbol, const std::string& wanted)
{
    return relocationSymbol != nullptr && wanted == relocationSymbol;
}

struct Search {
    std::string protectionError;
    const std::string* object = nullptr;
    const std::string* symbol = nullptr;
    GotSite* found = nullptr;
    std::vector<std::string>* names = nullptr;
    bool sawObject = false;
};

int visit(dl_phdr_info* object, std::size_t, void* opaque)
{
    auto& search = *static_cast<Search*>(opaque);
    if (!namesObject(object->dlpi_name, *search.object)) {
        return 0;
    }
    search.sawObject = true;

    const DynamicTables tables = tablesOf(*object);
    if (!tables.complete()) {
        return 1;
    }

    const std::size_t count = tables.relocationBytes / sizeof(ElfW(Rela));
    for (std::size_t i = 0; i < count; ++i) {
        const ElfW(Rela)& relocation = tables.relocations[i];
        if (ELF64_R_TYPE(relocation.r_info) != R_X86_64_JUMP_SLOT) {
            continue;
        }
        const ElfW(Sym)& symbol = tables.symbols[ELF64_R_SYM(relocation.r_info)];
        const char* name = tables.strings + symbol.st_name;

        if (search.names != nullptr) {
            search.names->emplace_back(name);
            continue;
        }
        if (!namesSymbol(name, *search.symbol)) {
            continue;
        }

        auto** slot = reinterpret_cast<void**>(object->dlpi_addr + relocation.r_offset);
        search.found->caller = object->dlpi_name != nullptr ? object->dlpi_name : "";
        search.found->symbol = name;
        search.found->slot = slot;
        search.found->resolved = *slot;
        // Recorded at resolve time, so a restore puts back what the loader
        // chose. Failing to read it fails the resolve: defaulting to read-only
        // is how the permissions get downgraded, which is the thing this field
        // exists to prevent, and a site that cannot be restored correctly is
        // not one anybody should be handed.
        if (!pageProtectionOf(slot, search.found->pageProtection, search.protectionError)) {
            search.found->slot = nullptr;
            return 1;
        }
        return 1;
    }
    return 1;
}

// Writes one pointer into a mapping the loader left read-only.
//
// The page is made writable, the store happens, and the permission goes back.
// Failing to put it back leaves the slot correct and the page writable, which
// is worth saying: the call is redirected as asked, and something that should
// be read-only is not.
// Writes one pointer, leaving the mapping as the loader had it.
//
// Making it writable is skipped where it already is, which is the common case
// for a lazily bound object and the case where forcing it back to read-only
// would break the next lazy resolution on that page.
bool storeIntoSlot(void** slot, void* value, const int protection, std::string& error)
{
    const auto pageSize = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    const auto address = reinterpret_cast<std::uintptr_t>(slot);
    auto* page = reinterpret_cast<void*>(address & ~(pageSize - 1));
    // A slot is one aligned pointer, so it never straddles pages; the page it
    // sits in is what has to be writable.
    const std::size_t span = pageSize;
    const bool alreadyWritable = (protection & PROT_WRITE) != 0;

    if (!alreadyWritable && ::mprotect(page, span, protection | PROT_WRITE) != 0) {
        error = std::string("could not make the slot writable: ") + std::strerror(errno);
        return false;
    }

    // One aligned pointer word, stored atomically, so a caller loading it gets
    // the old destination or the new one and never a mixture.
    static_assert(std::atomic_ref<void*>::is_always_lock_free);
    std::atomic_ref<void*>(*slot).store(value, std::memory_order_release);

    if (!alreadyWritable && ::mprotect(page, span, protection) != 0) {
        error = std::string("the slot was written and the mapping could not be put back to "
                            "the permissions the loader chose: ")
            + std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace

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
        error = std::string("could not read this process's mappings: ") + std::strerror(errno);
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

bool resolveGotSlot(const std::string& callerObject,
                    const std::string& symbol,
                    GotSite& site,
                    std::string& error)
{
    site = GotSite{};
    if (symbol.empty()) {
        error = "a symbol name is required";
        return false;
    }

    Search search;
    search.object = &callerObject;
    search.symbol = &symbol;
    search.found = &site;
    ::dl_iterate_phdr(&visit, &search);

    if (!search.sawObject) {
        error = "no loaded object named '" + callerObject
            + "'; the name is matched by suffix, so a versioned file name works";
        return false;
    }
    if (!search.protectionError.empty()) {
        error = "the slot for '" + symbol + "' was found and what the loader had done to its "
                "page could not be read, so restoring it later could not put that back: "
            + search.protectionError;
        return false;
    }
    if (!site.valid()) {
        error = "'" + (callerObject.empty() ? std::string("the main executable") : callerObject)
            + "' does not call '" + symbol
            + "' through its table. Either it never calls it, or the call is inside the "
              "object that defines it, where there is no relocation to redirect";
        return false;
    }
    return true;
}

std::vector<std::string> callableSymbols(const std::string& callerObject, std::string& error)
{
    std::vector<std::string> names;
    Search search;
    search.object = &callerObject;
    search.names = &names;
    ::dl_iterate_phdr(&visit, &search);
    if (!search.sawObject) {
        error = "no loaded object named '" + callerObject + "'";
    }
    return names;
}

bool redirectGotSlot(const GotSite& site, void* const replacement, std::string& error)
{
    if (!site.valid()) {
        error = "the site was never resolved";
        return false;
    }
    if (replacement == nullptr) {
        error = "a replacement is required";
        return false;
    }
    return storeIntoSlot(site.slot, replacement, site.pageProtection, error);
}

bool restoreGotSlot(const GotSite& site, std::string& error)
{
    if (!site.valid()) {
        error = "the site was never resolved";
        return false;
    }
    return storeIntoSlot(site.slot, site.resolved, site.pageProtection, error);
}

} // namespace runtime_agent
