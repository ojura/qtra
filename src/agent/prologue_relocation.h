#pragma once

// Redirecting a function that reserved no space at its entry, by moving the
// instructions that were there.
//
// The entry patcher needs an area the compiler set aside, which means building
// the object that defines the target. The table patcher needs a call that
// crosses an object boundary, so a relocation exists to point elsewhere. A
// function called only from inside its own object, built without the flag, is
// reachable by neither.
//
// What this does: copies the first whole instructions of the target somewhere
// else, writes a jump to the replacement over them, and ends the copy with a
// jump back to the instruction after the ones it took. The copy is what a
// replacement calls to reach the original behaviour.
//
// A thread already executing past those first bytes keeps running the original,
// which is not a defect but is worth knowing: this changes what a call reaches,
// and says nothing about calls already under way.
//
// THREE HAZARDS, and refusing is an answer to any of them.
//
// Instruction boundaries. Taking half an instruction leaves the rest as the
// tail of whatever is written over it. The decoder below recognises a listed
// subset and refuses everything else, so a length is never guessed.
//
// Position dependence. An instruction that names an address relative to where
// it sits means something different once moved. RIP-relative operands are
// adjusted, since the copy's address is known and the correction is arithmetic.
// Relative branches are refused: a branch out of a prologue is rare, and one
// whose destination is inside the range being taken is the next hazard wearing
// different clothes.
//
// Something jumping back into the bytes taken. A loop later in the function
// whose target is inside the overwritten range lands in the middle of the
// written jump and executes whatever that leaves. Nothing about the entry shows
// this, so the whole body is swept and every branch target collected. A target
// strictly inside the taken range refuses the site.
//
// The sweep decodes from the entry forward and assumes what follows an
// instruction is another instruction. That is not true of arbitrary code: data
// mixed into a function's body, or a jump table the compiler laid inline, will
// be decoded as instructions and the answer will be wrong. What makes it usable
// here is that undecodable bytes refuse instead of being skipped, so the sweep
// either covers the whole body with instructions it recognises or gives up.
//
// A LEASE COVERS ONE OF THESE AND NOT THE OTHER. The bytes being overwritten
// are the bytes no thread may be standing inside, and also the bytes nothing
// may jump into. The first is a question about this instant that a quiescence
// policy can answer. The second is a question about the code itself that no
// policy can, which is why the sweep exists.

#include "agent/entry_hotpatch.h"
#include "agent/quiescence.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace runtime_agent {

// One instruction, as much as is needed to move it or refuse it.
//
// The forms decoded are the ones compilers emit in a function's opening bytes,
// listed so the boundary of what is understood is visible:
//
//   push and pop of a 64-bit register            50+r, 58+r, with or without REX
//   mov between registers and memory             88, 89, 8A, 8B
//   mov of an immediate                          C6, C7, and B8+r including movabs
//   lea                                          8D
//   add, or, and, sub, xor, cmp, register forms  01, 03, 09, 0B, 21, 23, 29, 2B, 31, 33, 39, 3B
//   the same against an immediate                81 and 83
//   test                                         85
//   nop, in its one-byte and multi-byte forms    90 and 0F 1F
//   endbr64                                      F3 0F 1E FA
//   movzx and movsx                              0F B6, B7, BE, BF
//   ret                                          C3
//   the indirect group                           FF
//   call and jmp taking a relative displacement  E8, E9, EB, 70+cc, 0F 80+cc
//
// Anything else is refused. A length guessed wrong is worse than a refusal,
// because it is silent.
struct DecodedInstruction {
    std::size_t length = 0;

    // Names an address as a distance from the end of this instruction, so
    // moving it changes what it reaches.
    bool ripRelative = false;
    std::size_t displacementOffset = 0;

    // A branch whose destination is written as a distance. Collected for the
    // sweep, refused inside the range being taken.
    bool relativeBranch = false;

    // Whether this hands control somewhere, by any means: a call or jump
    // through a register or memory, a return, or an entry into the kernel.
    //
    // Separate from relativeBranch, which is about an instruction that would
    // mean something different somewhere else. This is about an instruction
    // that can put a thread outside these bytes with a way back into them. A
    // thread parked in a callee has its return address in the range about to be
    // overwritten, and no policy that reads instruction pointers can see it.
    //
    // The sweep cannot help either: where an indirect branch goes is decided at
    // run time, so a scan cannot prove it does not enter the taken bytes.
    bool transfersControl = false;

    // Whether where it goes cannot be established by reading the function.
    //
    // An indirect jump takes its destination from a register or memory at the
    // moment it runs. A sweep can see that the instruction is there and can say
    // nothing about where it lands, so a body containing one is a body in which
    // nothing can be shown not to enter the bytes being taken. An indirect call
    // is different in one way that matters: it returns, so what it threatens is
    // a return address and not an entry, which is why the two are told apart.
    bool unprovableTarget = false;
    const std::uint8_t* branchTarget = nullptr;
};

// Reads one instruction. Refuses anything outside the list above.
[[nodiscard]] bool decodeInstruction(const std::uint8_t* at,
                                     DecodedInstruction& decoded,
                                     std::string& error);

// What was found at a function's entry, and where a jump can go.
//
// Planning writes nothing. Every refusal is available before any byte changes,
// which is what lets a caller ask whether a function can be redirected without
// finding out by damaging it.
struct ProloguePlan {
    void* entry = nullptr;

    // Where the jump goes, which is past the landing pad when there is one.
    // Overwriting an ENDBR64 would leave every indirect call to this function
    // faulting on a machine enforcing it.
    void* patchAddress = nullptr;

    // How many bytes of whole instructions are taken. At least enough for the
    // jump, and rounded up to the end of the last instruction it reaches into.
    std::size_t takenBytes = 0;

    // The function's whole extent, from the symbol table, which the sweep needs.
    std::size_t functionBytes = 0;

    bool keepsLandingPad = false;
    bool adjustedRipRelative = false;

    [[nodiscard]] bool valid() const noexcept { return patchAddress != nullptr; }
};

// Whether this function's opening instructions can be moved, and which ones.
//
// The extent comes from the dynamic symbol table through dladdr, so a function
// the loader cannot name is refused: without knowing where the body ends there
// is no way to sweep it, and no way to answer the third hazard.
[[nodiscard]] bool planPrologueRelocation(void* function,
                                          ProloguePlan& plan,
                                          std::string& error);

// A prologue that has been moved, and everything needed to put it back.
struct RelocatedPrologue {
    ProloguePlan plan;

    // Runs the instructions that were at the entry and then continues into the
    // rest of the function. This is what a replacement calls to reach the
    // original, and it begins with a landing pad because calling it through a
    // pointer is an indirect branch.
    void* original = nullptr;

    void* trampoline = nullptr;
    std::size_t trampolineBytes = 0;

    // What the entry held, so restoring is a copy and not a reconstruction.
    std::vector<std::uint8_t> savedBytes;
};

// Moves the prologue and points the entry at the replacement.
//
// The quiescer is asked to stop execution for the one write, because several
// bytes change and a thread standing inside them would execute the join of what
// was there and what is arriving. The range being written is the range that
// matters, and a policy that can account for threads should be told it.
[[nodiscard]] bool installRelocatedPrologue(const ProloguePlan& plan,
                                            void* replacement,
                                            Quiescer& quiescer,
                                            TextWriter& writer,
                                            RelocatedPrologue& installed,
                                            std::string& error);

// Puts the entry's own bytes back. The trampoline is left mapped, because a
// replacement may still hold the address it handed out and a thread may be
// inside it.
[[nodiscard]] bool restoreRelocatedPrologue(const RelocatedPrologue& installed,
                                            Quiescer& quiescer,
                                            TextWriter& writer,
                                            std::string& error);

} // namespace runtime_agent
