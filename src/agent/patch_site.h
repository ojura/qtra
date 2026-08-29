#pragma once

// Where a function may be patched.
//
// GCC's -fpatchable-function-entry reserves an area at a function's entry and
// records the address of that area in a __patchable_function_entries section.
// The section's name is a valid C identifier, so the linker defines
// __start___patchable_function_entries and __stop___patchable_function_entries
// for anything that refers to them, and the addresses are relocated for the
// running image. Reading it needs no file access and no section headers.
//
// The section is authoritative for where to write and says nothing about how
// much room is there. It is an address list with no lengths, so the amount is
// declared by the build that set the flag and confirmed against the image.

#include <cstddef>
#include <string>

namespace runtime_agent {

// One resolved site, valid for the image that produced it and for as long as
// nothing has been written there.
//
// availableBytes is a measurement taken at resolve time: the count of untouched
// NOPs starting at patchAddress. Installing anything destroys the evidence, so
// a site is a snapshot. Resolve before writing, carry the result, and confirm it
// again under quiescence.
struct PatchSite {
    void* entry = nullptr;
    void* patchAddress = nullptr;
    std::size_t availableBytes = 0;

    // The patch area follows a CET landing pad, so anything reached by an
    // indirect branch from here must itself begin with ENDBR64.
    bool requiresEndbr64 = false;

    // Reporting only. What is at the address decides whether a site is usable.
    std::string name;
    std::string preparedBy;

    [[nodiscard]] bool valid() const noexcept
    {
        return entry != nullptr && patchAddress != nullptr && availableBytes > 0;
    }
};

// Resolves the site for a function in the main executable, given how many bytes
// the build reserved for it.
//
// declaredBytes comes from the build that set -fpatchable-function-entry, which
// is the only place the amount is stated. The image is measured and the two are
// compared.
//
// Every failure is a refusal with a distinct reason. An image that prepared
// nothing and a function absent from what an image did prepare are separate
// problems with separate answers. Only the main executable is covered: the
// bounds symbols describe one object's section, and a function in a shared
// library sits in a section they do not reach.
[[nodiscard]] bool resolvePatchSite(void* function,
                                    std::size_t declaredBytes,
                                    PatchSite& site,
                                    std::string& error);

} // namespace runtime_agent
