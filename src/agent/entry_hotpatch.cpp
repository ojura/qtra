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

TextWriteResult writeText(void* address,
                          const std::uint8_t* bytes,
                          const std::size_t size,
                          const ProtectFunction protect)
{
    TextWriteResult result;
#if !defined(__linux__) || !defined(__x86_64__)
    (void)address;
    (void)bytes;
    (void)size;
    (void)protect;
    result.error = "writing mapped text is implemented only for Linux/x86-64";
    return result;
#else
    if (address == nullptr || bytes == nullptr || size == 0) {
        result.error = "invalid write request";
        return result;
    }

    const long page_size_long = ::sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0) {
        result.error = errnoMessage("sysconf(_SC_PAGESIZE)");
        return result;
    }
    const auto page_size = static_cast<std::uintptr_t>(page_size_long);
    const auto target_address = reinterpret_cast<std::uintptr_t>(address);
    const auto page_begin = target_address & ~(page_size - 1U);

    if (size > std::numeric_limits<std::uintptr_t>::max() - target_address) {
        result.error = "address range overflow";
        return result;
    }
    const auto end_address = target_address + size;
    const auto page_end = (end_address + page_size - 1U) & ~(page_size - 1U);
    const auto mapping_size = static_cast<std::size_t>(page_end - page_begin);

    auto* mapping = reinterpret_cast<void*>(page_begin);
    const ProtectFunction change = protect != nullptr ? protect : &::mprotect;

    if (change(mapping, mapping_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        result.error = errnoMessage("mprotect(RWX)");
        return result;
    }

    std::memcpy(address, bytes, size);
    __builtin___clear_cache(static_cast<char*>(address),
                            static_cast<char*>(address) + size);

    // Past this point the bytes have changed, so every exit reports that. The
    // demo target lives in the executable's text segment; a patcher for
    // arbitrary mappings should record and restore each one's prior
    // permissions instead of assuming these.
    if (change(mapping, mapping_size, PROT_READ | PROT_EXEC) != 0) {
        result.outcome = TextWriteOutcome::WrittenProtectionNotRestored;
        result.error = errnoMessage("mprotect(RX)");
        return result;
    }

    result.outcome = TextWriteOutcome::Written;
    return result;
#endif
}

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

    const TextWriteResult write = writeText(m_patchAddress, patch.data(), patch.size(), m_protect);
    if (write.complete()) {
        m_state = PatchState::Active;
        return true;
    }

    error = write.error;
    if (!write.changedBytes()) {
        m_state = PatchState::Inactive;
        m_target = nullptr;
        m_patchAddress = nullptr;
        m_replacement = nullptr;
        m_original.clear();
        return false;
    }

    // The entry now holds the jump and the mapping could not be put back. The
    // replacement is reachable, so the saved bytes are the only route to the
    // original and are kept. A caller that treats this as "nothing happened"
    // resumes a target that is already redirected.
    m_state = PatchState::RecoveryRequired;
    error += "; the entry was already rewritten, so the replacement is live and "
             "rollback is still required";
    return false;
#endif
}

bool EntryHotpatch::rollback(std::string& error)
{
    error.clear();
    if (!active()) {
        error = "no entry patch is active";
        return false;
    }

    const TextWriteResult write =
        writeText(m_patchAddress, m_original.data(), m_original.size(), m_protect);
    if (!write.changedBytes()) {
        // The entry still holds the jump, so the patch is still in force.
        error = write.error;
        return false;
    }

    // The saved bytes are back, so the target runs its own instructions again
    // whatever else went wrong.
    m_state = PatchState::Inactive;
    m_target = nullptr;
    m_patchAddress = nullptr;
    m_replacement = nullptr;
    m_original.clear();

    if (!write.complete()) {
        error = write.error;
        error += "; the entry was restored, so the target is no longer "
                 "redirected, and the mapping is left writable";
        return false;
    }
    return true;
}

} // namespace runtime_agent
