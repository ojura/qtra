#pragma once

#include "agent/patch_site.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace runtime_agent {

// What a write into mapped text did, which is a different question from whether
// it succeeded. Writing runs as three steps: make the mapping writable, copy,
// put the permissions back. A failure in the third step leaves bytes changed,
// so a caller told only "false" would carry on as though the process still ran
// the code it did before.
enum class TextWriteOutcome {
    NotWritten,
    Written,
    WrittenProtectionNotRestored,
};

struct TextWriteResult {
    TextWriteOutcome outcome = TextWriteOutcome::NotWritten;
    std::string error;

    [[nodiscard]] bool complete() const noexcept
    {
        return outcome == TextWriteOutcome::Written;
    }

    // Whether the bytes at the address may differ from what was there before.
    [[nodiscard]] bool changedBytes() const noexcept
    {
        return outcome != TextWriteOutcome::NotWritten;
    }
};

// The permission call, as a parameter so a test can fail the one after the copy.
// A real mprotect will not fail on demand, and that is the path worth testing,
// because it is the only one that leaves the process changed and the caller
// told nothing worked.
using ProtectFunction = int (*)(void* address, std::size_t length, int protection);

// Copies bytes over mapped executable text. Knows nothing about what the bytes
// mean, so gateway installation and entry redirection share it unchanged.
[[nodiscard]] TextWriteResult writeText(void* address,
                                        const std::uint8_t* bytes,
                                        std::size_t size,
                                        ProtectFunction protect = nullptr);

// How something that installs code writes it, so what does the writing is a
// choice its owner makes and not a fact about the type.
//
// The state worth testing is a copy that happened followed by a mapping that
// could not be restored, because it is the only one that leaves the process
// changed while the caller is told nothing worked. A real mprotect does not
// fail on demand, so reaching it means supplying a writer that reports it.
using TextWriter = TextWriteResult (*)(void* address,
                                       const std::uint8_t* bytes,
                                       std::size_t size);

// Writes into this process's own mapped text, which is what an owner that was
// given no writer of its own uses.
[[nodiscard]] TextWriteResult writeMappedText(void* address,
                                              const std::uint8_t* bytes,
                                              std::size_t size);


// How a gateway is laid out inside a prepared area.
//
// The gateway loads its destination from a slot and jumps through it, so the
// bytes are written once and every later change is a store to the slot. The
// continuation is an ENDBR64 immediately after it: the slot starts pointing
// there, so a call with nothing installed goes gateway, landing pad, remaining
// NOPs, original body. It is a valid indirect-branch target, which the jump
// requires under CET.
//
//   prepared[0..9]    movabs r11, <slot address>
//   prepared[10..12]  jmp qword ptr [r11]
//   prepared[13..16]  endbr64, the original continuation
//   prepared[17..]    NOPs, falling through to the function's own instructions
struct GatewayLayout {
    static constexpr std::size_t jumpBytes = 13;
    static constexpr std::size_t continuationBytes = 4;
    static constexpr std::size_t totalBytes = jumpBytes + continuationBytes;

    // Where the slot points when nothing is installed.
    [[nodiscard]] static void* continuationAddress(void* patchAddress) noexcept
    {
        return static_cast<std::uint8_t*>(patchAddress) + jumpBytes;
    }
};

// Builds the bytes for a gateway that jumps through the given slot. The area is
// filled to its full length so nothing of what was there survives between the
// continuation and the function body.
[[nodiscard]] std::vector<std::uint8_t> encodeGateway(void* slotAddress,
                                                      std::size_t areaBytes);

// Whether a gateway can go here, checked against the bytes actually present.
// The site was measured earlier, so this confirms it has not gone stale, and it
// runs with execution stopped.
[[nodiscard]] bool siteAcceptsGateway(const PatchSite& site, std::string& error);

// Whether the gateway's indirect jump may land on this address. Under CET an
// indirect branch is only allowed to reach an ENDBR64.
[[nodiscard]] bool replacementIsReachable(const PatchSite& site,
                                          const void* replacement,
                                          std::string& error);

} // namespace runtime_agent
