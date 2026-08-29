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

constexpr std::uint8_t endbr64[]{0xF3U, 0x0FU, 0x1EU, 0xFAU};

std::string addressString(const void* address)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(address);
    return stream.str();
}

// Whether a recorded point belongs to this function.
//
// Proximity alone would accept the entry of whatever function follows a short
// one. The compiler records either the function's own address, or four bytes in
// when a CET landing pad comes first, so those are the two answers that can be
// checked against the bytes present.
bool associatedWith(const std::uint8_t* function,
                    const std::uintptr_t recorded,
                    bool& behindLandingPad)
{
    const auto address = reinterpret_cast<std::uintptr_t>(function);
    if (recorded == address) {
        behindLandingPad = false;
        return true;
    }
    if (recorded == address + sizeof(endbr64)
        && __builtin_memcmp(function, endbr64, sizeof(endbr64)) == 0) {
        behindLandingPad = true;
        return true;
    }
    return false;
}

} // namespace

bool runtime_agent::resolvePatchSite(void* function,
                                     const std::size_t declaredBytes,
                                     PatchSite& site,
                                     std::string& error)
{
    site = PatchSite{};
    if (function == nullptr || declaredBytes == 0) {
        error = "a function and the number of bytes its build reserved are both required";
        return false;
    }

    // Taken as pointers before anything is compared, because the linker gives
    // these as arrays and comparing two arrays directly is deprecated in C++20.
    // Weak, so an image that prepared nothing links and leaves them null.
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

    const auto* bytes = static_cast<const std::uint8_t*>(function);
    std::uintptr_t recorded = 0;
    bool behindLandingPad = false;
    for (std::size_t index = 0; index < count; ++index) {
        if (associatedWith(bytes, entries[index], behindLandingPad)) {
            recorded = entries[index];
            break;
        }
    }

    if (recorded == 0) {
        error = "the function at " + addressString(function)
              + " is not among this program's prepared entries, so it was built without "
                "-fpatchable-function-entry and has no reserved area to write into";
        return false;
    }

    // Bounded by what the build says it reserved. Counting until the NOPs stop
    // would run into whatever follows the area, and ordinary code and alignment
    // padding can both begin with NOPs.
    const auto* area = reinterpret_cast<const std::uint8_t*>(recorded);
    std::size_t available = 0;
    while (available < declaredBytes && area[available] == 0x90U) {
        ++available;
    }
    if (available < declaredBytes) {
        error = "the area at " + addressString(area) + " holds " + std::to_string(available)
              + " of the " + std::to_string(declaredBytes)
              + " bytes this build reserved, so something is installed there already or "
                "this program was built with a different reservation than the one stated";
        return false;
    }

    site.entry = function;
    site.patchAddress = const_cast<std::uint8_t*>(area);
    site.availableBytes = available;
    site.requiresEndbr64 = behindLandingPad;
    site.preparedBy = "__patchable_function_entries";
    return true;
}

#else

bool runtime_agent::resolvePatchSite(void*, std::size_t, PatchSite& site, std::string& error)
{
    site = PatchSite{};
    error = "resolving patch sites is implemented only for Linux/x86-64";
    return false;
}

#endif
