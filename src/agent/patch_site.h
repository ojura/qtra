#pragma once

// Where a function may be patched, answered by what the compiler recorded
// rather than by what the caller believes.
//
// GCC's -fpatchable-function-entry reserves an area at a function's entry and
// writes the address of that area into a __patchable_function_entries section.
// The section's name is a valid C identifier, so the linker defines
// __start___patchable_function_entries and __stop___patchable_function_entries
// for anything that refers to them, and the addresses in it are relocated for
// the running image. Reading it therefore needs no file access and no section
// header walk.
//
// The recorded address already accounts for an Intel CET landing pad: for a
// function beginning with ENDBR64 it points four bytes in, at the first
// reserved byte. Deriving that instead, by testing whether a function starts
// with those four bytes, guesses at something the compiler already knows.

#include <cstddef>
#include <string>

namespace runtime_agent {

// A resolved answer, valid for the image that produced it.
//
// reservedBytes is measured when the site is resolved and carried afterwards,
// because it is measured by counting the untouched NOPs and anything installed
// at the site destroys the evidence. A site is a snapshot: resolve before
// installing, not after.
struct PatchSite {
    void* function = nullptr;
    void* patchAddress = nullptr;
    std::size_t reservedBytes = 0;
    std::string provenance;

    [[nodiscard]] bool valid() const noexcept
    {
        return function != nullptr && patchAddress != nullptr && reservedBytes > 0;
    }
};

// Resolves the site for a function in the main executable.
//
// Refuses rather than guesses. A function with no recorded entry was not
// prepared, and that is reported differently from a build where nothing at all
// was prepared, because the first is a question about one function and the
// second is a question about how the program was compiled.
//
// Only the main executable is covered. The linker-provided bounds describe one
// object's section, so a function in a shared library has its own section that
// these symbols do not reach.
[[nodiscard]] bool resolvePatchSite(void* function, PatchSite& site, std::string& error);

} // namespace runtime_agent
