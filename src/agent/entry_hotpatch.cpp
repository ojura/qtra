#include "agent/entry_hotpatch.h"

#include "agent/errno_text.h"
#include "agent/page_span.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <string>

#if defined(__linux__)
#  include <sys/mman.h>
#  include <unistd.h>
#endif

namespace runtime_agent {

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
    result.failedOperation = "writing mapped text is implemented only for Linux/x86-64";
    return result;
#else
    if (address == nullptr || bytes == nullptr || size == 0) {
        result.failedOperation = "invalid write request";
        return result;
    }

    const std::size_t page_size = pageSize();
    if (page_size == 0) {
        result.failedOperation = "sysconf(_SC_PAGESIZE)";
        result.failureErrno = errno;
        return result;
    }
    const auto target_address = reinterpret_cast<std::uintptr_t>(address);

    std::uintptr_t page_begin = 0;
    std::size_t mapping_size = 0;
    if (!pageSpan(target_address, size, page_size, page_begin, mapping_size)) {
        result.failedOperation = "address range overflow";
        return result;
    }

    auto* mapping = reinterpret_cast<void*>(page_begin);
    const ProtectFunction change = protect != nullptr ? protect : &::mprotect;

    if (change(mapping, mapping_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        result.failedOperation = "mprotect(RWX)";
        result.failureErrno = errno;
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
        result.failedOperation = "mprotect(RX)";
        result.failureErrno = errno;
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
    static_assert(sizeof(endbr64Bytes) == 4);
    std::memcpy(bytes.data() + 13, endbr64Bytes, sizeof(endbr64Bytes));
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

bool processRequiresLandingPads() noexcept
{
    // GCC and Clang set bit 1 of __CET__ for -fcf-protection=branch and =full.
#if defined(__CET__) && ((__CET__ & 2) != 0)
    return true;
#else
    return false;
#endif
}

bool replacementIsReachable(const PatchSite& site, const void* replacement, std::string& error)
{
    error.clear();
    if (replacement == nullptr) {
        error = "a replacement address is required";
        return false;
    }

    // Two reasons to require a landing pad, and either one is enough.
    //
    // site.requiresEndbr64 says the target function begins with one, which is a
    // fact about how that function was compiled. The process is the thing that
    // enforces branch tracking, and a function carrying nocf_check has no pad
    // while tracking is still on around it, so the site alone admits a
    // replacement that faults on the first call through the gateway.
    //
    // What the build asked for answers the process question. __CET__ carries
    // bit 1 where -fcf-protection turned branch tracking on, and it is the same
    // build that writes the indirect jump this rule is about. Where the loader
    // has switched tracking off because some object lacks the property, this
    // asks for a pad that nothing checks, which costs a replacement that would
    // have worked and never admits one that faults.
    const bool processTracksBranches = processRequiresLandingPads();
    if ((site.requiresEndbr64 || processTracksBranches)
        && std::memcmp(replacement, endbr64Bytes, sizeof(endbr64Bytes)) != 0) {
        error = processTracksBranches
            ? "this build turned on indirect branch tracking, and the gateway reaches a "
              "replacement through an indirect jump, so the replacement has to begin with "
              "ENDBR64"
            : "this entry sits behind a CET landing pad, so the jump to it is indirect "
              "and anything it reaches has to begin with ENDBR64";
        return false;
    }
    return true;
}

} // namespace runtime_agent
