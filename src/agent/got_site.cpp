#include "agent/got_site.h"

#include <cerrno>
#include <cstdint>
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

// Whether a relocation's symbol is the one asked for, with or without the
// version tag the library attached to it.
bool namesSymbol(const char* relocationSymbol, const std::string& wanted)
{
    if (relocationSymbol == nullptr) {
        return false;
    }
    const std::string name = relocationSymbol;
    if (name == wanted) {
        return true;
    }
    const std::size_t at = name.find('@');
    return at != std::string::npos && name.compare(0, at, wanted) == 0;
}

struct Search {
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
bool storeIntoSlot(void** slot, void* value, std::string& error)
{
    const auto pageSize = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    auto address = reinterpret_cast<std::uintptr_t>(slot);
    auto* page = reinterpret_cast<void*>(address & ~(pageSize - 1));
    // A slot never straddles pages, since it is one aligned pointer, but the
    // page it sits in is what has to be made writable.
    const std::size_t span = pageSize;

    if (::mprotect(page, span, PROT_READ | PROT_WRITE) != 0) {
        error = std::string("could not make the slot writable: ") + std::strerror(errno);
        return false;
    }

    *slot = value;

    if (::mprotect(page, span, PROT_READ) != 0) {
        error = std::string("the slot was written and the mapping could not be made "
                            "read-only again: ")
            + std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace

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
    return storeIntoSlot(site.slot, replacement, error);
}

bool restoreGotSlot(const GotSite& site, std::string& error)
{
    if (!site.valid()) {
        error = "the site was never resolved";
        return false;
    }
    return storeIntoSlot(site.slot, site.resolved, error);
}

} // namespace runtime_agent
