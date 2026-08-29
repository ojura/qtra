#pragma once

#include "agent/errno_text.h"
#include "agent/patch_site.h"

#include <cstddef>
#include <cstdint>
#include <memory>
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

    // What went wrong, recorded without allocating.
    //
    // A write into live text happens with every other thread parked, and one of
    // them can be inside the allocator holding its lock. Building a sentence
    // here would wait for a thread that cannot run until the caller releases
    // them, which is a deadlock the caller cannot see. So a failure is kept as
    // a literal and an errno, and message() turns it into a sentence once the
    // caller has let the threads go.
    //
    // failedOperation is always a string literal, so nothing owns it.
    const char* failedOperation = nullptr;
    int failureErrno = 0;

    [[nodiscard]] std::string message() const
    {
        if (failedOperation == nullptr) {
            return {};
        }
        if (failureErrno == 0) {
            return std::string(failedOperation);
        }
        return std::string(failedOperation) + " failed: " + errnoText(failureErrno);
    }

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
//
// An object and not a function pointer, so a writer owns whatever it needs to
// answer differently on different calls. A test that counts its own writes then
// keeps that count to itself, and two tests running against one process do not
// share it.
//
// Held by shared_ptr, because what a writer writes into outlives most of what
// asks for a write. A gateway's storage is never reclaimed and an install that
// could not finish keeps its saved bytes until something recovers them, so the
// writer that recovery will use has to still be there when whoever installed
// the gateway is long gone. Sharing ownership is what makes that a fact instead
// of a rule somebody has to follow.
class TextWriter {
public:
    TextWriter() = default;
    TextWriter(const TextWriter&) = delete;
    TextWriter& operator=(const TextWriter&) = delete;
    virtual ~TextWriter() = default;

    // The writer is fixed for its owner's life; what it answers is per call.
    [[nodiscard]] virtual TextWriteResult write(void* address,
                                                const std::uint8_t* bytes,
                                                std::size_t size) = 0;
};

// Writes into this process's own mapped text, which is what an owner given no
// writer of its own uses.
class MappedTextWriter final : public TextWriter {
public:
    [[nodiscard]] TextWriteResult write(void* address,
                                        const std::uint8_t* bytes,
                                        std::size_t size) override;
};

// The one every owner shares unless it was given another.
[[nodiscard]] std::shared_ptr<TextWriter> processTextWriter();


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
// The site was measured earlier, so this confirms it has not gone stale.
//
// Two forms, and which one to call is decided by whether anything is stopped.
// The answer alone allocates nothing, so it is what runs with every thread
// parked: a parked thread may hold the allocator's lock, and building a
// sentence there is the caller waiting for a thread that is waiting for it.
// The other says why, and is for asking again once they are running.
[[nodiscard]] bool siteAcceptsGateway(const PatchSite& site) noexcept;
[[nodiscard]] bool siteAcceptsGateway(const PatchSite& site, std::string& error);

// Whether this process enforces indirect branch tracking, which decides whether
// anything the gateway jumps to needs a landing pad.
//
// Asked of the build rather than of the target's bytes. A function's own
// prologue says how that function was compiled, and nocf_check leaves a
// function without a pad in a process that still tracks branches.
[[nodiscard]] bool processRequiresLandingPads() noexcept;

// Whether the gateway's indirect jump may land on this address. Under CET an
// indirect branch is only allowed to reach an ENDBR64.
[[nodiscard]] bool replacementIsReachable(const PatchSite& site,
                                          const void* replacement,
                                          std::string& error);

} // namespace runtime_agent
