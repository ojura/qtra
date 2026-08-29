#pragma once

// Which pages a write covers.
//
// Three places worked this out for themselves: the text writer covering a range
// of instructions, the GOT writer covering one aligned pointer, and the copy
// allocator rounding a size up and finding the page a function starts on. Each
// asked sysconf again and built its own mask.
//
// Two of them cast the result straight to an unsigned type. sysconf reports
// failure as -1, so the size became SIZE_MAX, the mask built from it came out
// as 1, and the page base came out as 0. mprotect would then be asked to change
// a null address, which fails, but with an error about the arguments rather
// than about sysconf. Asked once here, and checked once here.

#include <cstddef>
#include <cstdint>

#include <unistd.h>

namespace runtime_agent {

// The system page size, or 0 if it cannot be determined.
//
// Asked once. It cannot change while the process runs, and the callers are on
// paths where a write into live code is about to happen.
[[nodiscard]] inline std::size_t pageSize() noexcept
{
    static const std::size_t size = [] {
        const long reported = ::sysconf(_SC_PAGESIZE);
        // A page size that is not a power of two would make the mask below
        // wrong, and every caller depends on that mask.
        if (reported <= 0) {
            return std::size_t{0};
        }
        const auto value = static_cast<std::size_t>(reported);
        return (value & (value - 1U)) == 0U ? value : std::size_t{0};
    }();
    return size;
}

// The first byte of the page holding this address.
[[nodiscard]] inline std::uintptr_t pageBase(const std::uintptr_t address,
                                             const std::size_t page) noexcept
{
    return address & ~(static_cast<std::uintptr_t>(page) - 1U);
}

// The pages covering [address, address + bytes), as a base and a length.
//
// False when the range wraps, which is the one way the rounding below can
// produce a span that does not cover what was asked for.
[[nodiscard]] inline bool pageSpan(const std::uintptr_t address,
                                   const std::size_t bytes,
                                   const std::size_t page,
                                   std::uintptr_t& base,
                                   std::size_t& span) noexcept
{
    if (page == 0) {
        return false;
    }
    const auto stride = static_cast<std::uintptr_t>(page);
    if (bytes > UINTPTR_MAX - address) {
        return false;
    }
    const std::uintptr_t end = address + bytes;
    if (end > UINTPTR_MAX - (stride - 1U)) {
        return false;
    }
    base = pageBase(address, page);
    span = static_cast<std::size_t>(pageBase(end + stride - 1U, page) - base);
    return true;
}

// A byte count rounded up to whole pages.
[[nodiscard]] inline bool roundUpToPages(const std::size_t bytes,
                                         const std::size_t page,
                                         std::size_t& rounded) noexcept
{
    if (page == 0 || bytes > SIZE_MAX - (page - 1U)) {
        return false;
    }
    rounded = (bytes + page - 1U) / page * page;
    return true;
}

} // namespace runtime_agent
