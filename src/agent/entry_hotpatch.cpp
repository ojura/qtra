#include "agent/entry_hotpatch.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <sstream>

#if defined(__linux__)
#  include <sys/mman.h>
#  include <unistd.h>
#endif

namespace runtime_agent {
namespace {

std::string errnoMessage(const char* operation)
{
    std::ostringstream stream;
    stream << operation << " failed: " << std::strerror(errno)
           << " (errno=" << errno << ')';
    return stream.str();
}

} // namespace

EntryHotpatch::~EntryHotpatch()
{
    if (active()) {
        std::string ignored;
        (void)rollback(ignored);
    }
}

bool EntryHotpatch::apply(void* target,
                          void* replacement,
                          const std::size_t reserved_bytes,
                          std::string& error)
{
    error.clear();
#if !defined(__linux__) || !defined(__x86_64__)
    (void)target;
    (void)replacement;
    (void)reserved_bytes;
    error = "raw entry hotpatching is implemented only for Linux/x86-64";
    return false;
#else
    if (target == nullptr || replacement == nullptr) {
        error = "target and replacement must both be non-null";
        return false;
    }
    if (active()) {
        error = "a patch is already active";
        return false;
    }

    // movabs r11, imm64 ; jmp r11
    // r11 is caller-saved under the System V AMD64 ABI.
    constexpr std::size_t jump_size = 13;
    if (reserved_bytes < jump_size) {
        error = "the reserved entry area is too small; at least 13 bytes are required";
        return false;
    }
    if (reserved_bytes > 4096) {
        error = "unreasonable reserved entry size";
        return false;
    }

    constexpr std::uint8_t endbr64[]{0xF3U, 0x0FU, 0x1EU, 0xFAU};
    auto* targetBytes = static_cast<std::uint8_t*>(target);
    std::size_t patchOffset = 0;
    if (std::memcmp(targetBytes, endbr64, sizeof(endbr64)) == 0) {
        // Preserve Intel CET/IBT's valid indirect-branch landing pad. Calls
        // land on ENDBR64 and then fall through into the replacement jump.
        patchOffset = sizeof(endbr64);
        if (std::memcmp(replacement, endbr64, sizeof(endbr64)) != 0) {
            error = "CET/IBT target requires a replacement beginning with ENDBR64";
            return false;
        }
    }

    auto* patchAddress = targetBytes + patchOffset;
    std::vector<std::uint8_t> original(reserved_bytes);
    std::memcpy(original.data(), patchAddress, reserved_bytes);
    if (!std::all_of(original.begin(), original.end(), [](const std::uint8_t byte) {
            return byte == 0x90U;
        })) {
        error = "target entry is not the expected all-NOP patchable_function_entry area";
        return false;
    }

    m_target = target;
    m_patchAddress = patchAddress;
    m_replacement = replacement;
    m_original = std::move(original);

    std::vector<std::uint8_t> patch(reserved_bytes, 0x90U);
    patch[0] = 0x49U;
    patch[1] = 0xBBU;
    const auto address = reinterpret_cast<std::uintptr_t>(replacement);
    static_assert(sizeof(address) == 8);
    std::memcpy(patch.data() + 2, &address, sizeof(address));
    patch[10] = 0x41U;
    patch[11] = 0xFFU;
    patch[12] = 0xE3U;

    if (!writeBytes(patch.data(), patch.size(), error)) {
        m_target = nullptr;
        m_patchAddress = nullptr;
        m_replacement = nullptr;
        m_original.clear();
        return false;
    }
    return true;
#endif
}

bool EntryHotpatch::rollback(std::string& error)
{
    error.clear();
    if (!active()) {
        error = "no entry patch is active";
        return false;
    }

    if (!writeBytes(m_original.data(), m_original.size(), error)) {
        return false;
    }

    m_target = nullptr;
    m_patchAddress = nullptr;
    m_replacement = nullptr;
    m_original.clear();
    return true;
}

bool EntryHotpatch::writeBytes(const std::uint8_t* bytes,
                               const std::size_t size,
                               std::string& error)
{
#if !defined(__linux__) || !defined(__x86_64__)
    (void)bytes;
    (void)size;
    error = "unsupported platform";
    return false;
#else
    if (m_target == nullptr || m_patchAddress == nullptr || bytes == nullptr || size == 0) {
        error = "invalid write request";
        return false;
    }

    const long page_size_long = ::sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0) {
        error = errnoMessage("sysconf(_SC_PAGESIZE)");
        return false;
    }
    const auto page_size = static_cast<std::uintptr_t>(page_size_long);
    const auto target_address = reinterpret_cast<std::uintptr_t>(m_patchAddress);
    const auto page_begin = target_address & ~(page_size - 1U);

    if (size > std::numeric_limits<std::uintptr_t>::max() - target_address) {
        error = "address range overflow";
        return false;
    }
    const auto end_address = target_address + size;
    const auto page_end = (end_address + page_size - 1U) & ~(page_size - 1U);
    const auto mapping_size = static_cast<std::size_t>(page_end - page_begin);

    auto* mapping = reinterpret_cast<void*>(page_begin);
    if (::mprotect(mapping, mapping_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        error = errnoMessage("mprotect(RWX)");
        return false;
    }

    std::memcpy(m_patchAddress, bytes, size);
    __builtin___clear_cache(static_cast<char*>(m_patchAddress),
                            static_cast<char*>(m_patchAddress) + size);

    // The demo target lives in the executable's text segment. A production
    // patcher should query and restore the exact prior permissions per mapping.
    if (::mprotect(mapping, mapping_size, PROT_READ | PROT_EXEC) != 0) {
        error = errnoMessage("mprotect(RX)");
        return false;
    }
    return true;
#endif
}

} // namespace runtime_agent
