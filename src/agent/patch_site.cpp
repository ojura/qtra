#include "agent/patch_site.h"

#include <cstdint>
#include <sstream>

#if defined(__linux__) && defined(__x86_64__)

extern "C" {
// Defined by the linker for any object that refers to them, because the
// section's name is a valid C identifier. Declaring them is what causes them to
// exist; they are absent from the symbol table otherwise.
extern const std::uint8_t __start___patchable_function_entries[] __attribute__((weak));
extern const std::uint8_t __stop___patchable_function_entries[] __attribute__((weak));
}

namespace {

std::string addressString(const void* address)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(address);
    return stream.str();
}

// How far past a function's own address a recorded entry may sit and still
// belong to it. With a zero prefix count the compiler records either the
// function address or, where a CET landing pad comes first, four bytes in.
constexpr std::uintptr_t maxEntryOffset = 8;

} // namespace

bool runtime_agent::resolvePatchSite(void* function, PatchSite& site, std::string& error)
{
    site = PatchSite{};
    if (function == nullptr) {
        error = "no function to resolve";
        return false;
    }

    // Taken as pointers before anything is compared, because the linker gives
    // these as arrays and comparing two arrays directly is deprecated in C++20.
    // Weak, so an image that prepared nothing leaves them null rather than
    // failing to link.
    const std::uint8_t* first = __start___patchable_function_entries;
    const std::uint8_t* last = __stop___patchable_function_entries;
    if (first == nullptr || last == nullptr || last <= first) {
        error = "this program records no patchable function entries, so nothing in it was "
                "built with -fpatchable-function-entry and no function can be patched at "
                "its entry";
        return false;
    }

    const auto* entries = reinterpret_cast<const std::uintptr_t*>(first);
    const std::size_t count =
        static_cast<std::size_t>(last - first) / sizeof(std::uintptr_t);

    const auto target = reinterpret_cast<std::uintptr_t>(function);
    std::uintptr_t found = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uintptr_t entry = entries[index];
        if (entry >= target && entry - target <= maxEntryOffset) {
            // The nearest recorded entry at or after the function address. A
            // later one belongs to whatever function comes next.
            if (found == 0 || entry < found) {
                found = entry;
            }
        }
    }

    if (found == 0) {
        error = "no patchable entry is recorded for the function at " + addressString(function)
              + ", so it was not built with -fpatchable-function-entry and has no reserved "
                "area to write into";
        return false;
    }

    // Measured now, because installing anything here overwrites the evidence.
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(found);
    std::size_t reserved = 0;
    while (bytes[reserved] == 0x90U) {
        ++reserved;
    }
    if (reserved == 0) {
        error = "the area recorded for the function at " + addressString(function)
              + " does not hold the expected NOPs, so either something is already installed "
                "there or this is not the area the compiler reserved";
        return false;
    }

    site.function = function;
    site.patchAddress = reinterpret_cast<void*>(found);
    site.reservedBytes = reserved;
    site.provenance = "__patchable_function_entries";
    return true;
}

#else

bool runtime_agent::resolvePatchSite(void*, PatchSite& site, std::string& error)
{
    site = PatchSite{};
    error = "resolving patch sites is implemented only for Linux/x86-64";
    return false;
}

#endif
