#include "agent/entry_hotpatch.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>

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

TextWriteResult MappedTextWriter::write(void* address,
                                        const std::uint8_t* bytes,
                                        const std::size_t size)
{
    return writeText(address, bytes, size, nullptr);
}

std::shared_ptr<TextWriter> processTextWriter()
{
    static const std::shared_ptr<TextWriter> writer = std::make_shared<MappedTextWriter>();
    return writer;
}

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
    // permissions, which requires reading them first.
    if (change(mapping, mapping_size, PROT_READ | PROT_EXEC) != 0) {
        result.outcome = TextWriteOutcome::WrittenProtectionNotRestored;
        result.error = errnoMessage("mprotect(RX)");
        return result;
    }

    result.outcome = TextWriteOutcome::Written;
    return result;
#endif
}

std::vector<std::uint8_t> encodeGateway(void* slotAddress, const std::size_t areaBytes)
{
    std::vector<std::uint8_t> bytes;
    if (slotAddress == nullptr || areaBytes < GatewayLayout::totalBytes) {
        return bytes;
    }

    bytes.assign(areaBytes, 0x90U);

    // movabs r11, <slot address>. r11 is caller-saved under the System V AMD64
    // ABI, so a callee is free to clobber it and no caller depends on it here.
    bytes[0] = 0x49U;
    bytes[1] = 0xBBU;
    const auto slot = reinterpret_cast<std::uintptr_t>(slotAddress);
    static_assert(sizeof(slot) == 8);
    std::memcpy(bytes.data() + 2, &slot, sizeof(slot));

    // jmp qword ptr [r11]. The destination comes from memory, which is what
    // makes changing it a store instead of another code write.
    bytes[10] = 0x41U;
    bytes[11] = 0xFFU;
    bytes[12] = 0x23U;

    // endbr64, the continuation. Reached only through the indirect jump above,
    // so under CET it has to be a landing pad.
    bytes[13] = 0xF3U;
    bytes[14] = 0x0FU;
    bytes[15] = 0x1EU;
    bytes[16] = 0xFAU;
    return bytes;
}

// The answer, with nothing said about it and nothing allocated to say it.
//
// This runs with every thread parked, so it touches no string. The reasons live
// in the form below, which asks the same questions in the same order and is
// called once execution is running again.
bool siteAcceptsGateway(const PatchSite& site) noexcept
{
    if (!site.valid() || site.availableBytes < GatewayLayout::totalBytes) {
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(site.patchAddress);
    for (std::size_t index = 0; index < site.availableBytes; ++index) {
        if (bytes[index] != 0x90U) {
            return false;
        }
    }
    return true;
}

bool siteAcceptsGateway(const PatchSite& site, std::string& error)
{
    error.clear();
    if (!site.valid()) {
        error = "a resolved site is required";
        return false;
    }
    if (site.availableBytes < GatewayLayout::totalBytes) {
        error = "the prepared area holds " + std::to_string(site.availableBytes)
              + " bytes and a gateway with its continuation needs "
              + std::to_string(GatewayLayout::totalBytes);
        return false;
    }

    // Confirmed against what is there now. The site is a measurement taken
    // earlier and this is the last moment before the write.
    //
    // This is also what stops an instrumented target being overwritten.
    // Measuring a function usually costs a byte of it: a uprobe writes 0xCC
    // over an instruction, and only a hardware execution breakpoint leaves the
    // text alone, of which there are four. So a target being measured does not
    // hold its reserved NOPs, and refusing here is correct: measurement ends
    // before installation begins, and a probe left in place is a reason to stop
    // and not something to write through.
    const auto* bytes = static_cast<const std::uint8_t*>(site.patchAddress);
    for (std::size_t index = 0; index < site.availableBytes; ++index) {
        if (bytes[index] != 0x90U) {
            error = "the prepared area no longer holds the NOPs it was resolved with, so "
                    "something has been written there since";
            return false;
        }
    }
    return true;
}

bool replacementIsReachable(const PatchSite& site, const void* replacement, std::string& error)
{
    error.clear();
    if (replacement == nullptr) {
        error = "a replacement address is required";
        return false;
    }

    constexpr std::uint8_t endbr64[]{0xF3U, 0x0FU, 0x1EU, 0xFAU};
    if (site.requiresEndbr64
        && std::memcmp(replacement, endbr64, sizeof(endbr64)) != 0) {
        error = "this entry sits behind a CET landing pad, so the jump to it is indirect "
                "and anything it reaches has to begin with ENDBR64";
        return false;
    }
    return true;
}

} // namespace runtime_agent
