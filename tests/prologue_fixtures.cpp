// Functions with prologues chosen to reach each answer the planner can give.
//
// Written as assembly because the point is the exact opening bytes. A C++
// function compiled at whatever optimization the build asks for is not a
// fixture: the compiler decides its prologue, and it decides differently
// between builds, so a test written against one would pass or fail for reasons
// unrelated to what it names.
//
// Each carries a size, because the planner reads a function's extent from the
// symbol table and refuses a symbol that records none.

#include "tests/prologue_fixtures.h"

// The landing pad these fixtures open with, present exactly when the build asks
// for indirect branch tracking.
//
// The fixtures are assembly, so an unconditional endbr64 here would give them a
// pad in a build whose compiled code has none. A replacement compiled in that
// build then has no pad, the fixture's entry says one is required, and binding
// is refused for a difference between the fixture and the compiler rather than
// anything the code under test decided.
#if defined(__CET__) && ((__CET__ & 2) != 0)
#  define FIXTURE_LANDING_PAD "    endbr64\n"
#else
#  define FIXTURE_LANDING_PAD ""
#endif


// A prologue of whole instructions the decoder reads, with nothing that names
// an address as a distance and nothing branching back into its opening bytes.
// Returns its argument plus one.
asm(".text\n"
    ".globl fixtureAddsOne\n"
    ".type fixtureAddsOne, @function\n"
    "fixtureAddsOne:\n"
    FIXTURE_LANDING_PAD
    R"(    push %rbp
    mov  %rsp, %rbp
    mov  %edi, %eax
    add  $1, %eax
    pop  %rbp
    ret
.size fixtureAddsOne, .-fixtureAddsOne
)");

// The same shape, reading a counter through an operand that names its address
// as a distance from the instruction after it. Moving that instruction without
// correcting the distance would read whatever sits near the copy instead.
asm(".text\n"
    ".globl fixtureCounter\n"
    ".type fixtureCounter, @function\n"
    "fixtureCounter:\n"
    FIXTURE_LANDING_PAD
    R"(    mov  fixtureCounterStorage(%rip), %eax
    add  $1, %eax
    ret
.size fixtureCounter, .-fixtureCounter

.data
.globl fixtureCounterStorage
.type fixtureCounterStorage, @object
.size fixtureCounterStorage, 4
fixtureCounterStorage:
    .long 41
)");

// Opens with a floating-point instruction, which this decoder does not read.
// fldz is 0xd9 0xee, and nothing in the tables covers the x87 block, so its
// length is unknown and the planner has to refuse.
asm(".text\n"
    ".globl fixtureUndecodable\n"
    ".type fixtureUndecodable, @function\n"
    "fixtureUndecodable:\n"
    FIXTURE_LANDING_PAD
    R"(    fldz
    fstp %st(0)
    mov  $7, %eax
    ret
.size fixtureUndecodable, .-fixtureUndecodable
)");

// Opens with CPUID, whose length is known so the body sweep can pass it, but
// which has not been approved for copying into a trampoline. Planned and never
// called: the fixture exists to prove that decoding a form does not approve it.
asm(".text\n"
    ".globl fixtureKnownButUnapproved\n"
    ".type fixtureKnownButUnapproved, @function\n"
    "fixtureKnownButUnapproved:\n"
    FIXTURE_LANDING_PAD
    R"(    cpuid
    nop
    nop
    nop
    ret
.size fixtureKnownButUnapproved, .-fixtureKnownButUnapproved
)");

// Opens with a jump, which names where it goes as a distance from itself, so
// moving it would send it somewhere else.
asm(".text\n"
    ".globl fixtureOpensWithABranch\n"
    ".type fixtureOpensWithABranch, @function\n"
    "fixtureOpensWithABranch:\n"
    FIXTURE_LANDING_PAD
    R"(    jmp  1f
    nop
    nop
    nop
1:
    mov  $9, %eax
    ret
.size fixtureOpensWithABranch, .-fixtureOpensWithABranch
)");

// Something later in the body jumps back into the middle of the bytes a jump
// would be written over.
//
// The landing pad takes the first four bytes, so the jump would start at four.
// It needs five, which the two-byte xor and the three-byte add supply, so the
// bytes taken run from four to eight. The loop target sits at six, between
// those two instructions and squarely inside that range, so a jump written
// there would be entered three bytes in and the processor would run the tail of
// its own displacement.
asm(".text\n"
    ".globl fixtureBranchesIntoItsPrologue\n"
    ".type fixtureBranchesIntoItsPrologue, @function\n"
    "fixtureBranchesIntoItsPrologue:\n"
    FIXTURE_LANDING_PAD
    R"(    xor  %eax, %eax
2:
    add  $1, %eax
    cmp  $3, %eax
    jl   2b
    ret
.size fixtureBranchesIntoItsPrologue, .-fixtureBranchesIntoItsPrologue

# Loads a value into r11 among the bytes a jump would be written over, and reads
# it back afterwards. Anything that uses r11 to get from the copy back to the
# body returns a different answer, with nothing crashing to draw attention.
.globl fixtureKeepsAValueInR11
.type fixtureKeepsAValueInR11, @function
fixtureKeepsAValueInR11:
)"
    FIXTURE_LANDING_PAD
    R"(    mov  $0x5EED, %r11d
    nop
    nop
    nop
    nop
    nop
    nop
    mov  %r11d, %eax
    ret
.size fixtureKeepsAValueInR11, .-fixtureKeepsAValueInR11

# Calls through a register in the first bytes a jump would be written over.
#
# Planned and never run: the register holds nothing, so calling it would go
# somewhere arbitrary. What is being tested is that the planner refuses it, and
# setting the register up first would push the call past the bytes taken and
# test nothing.
.globl fixtureCallsThroughARegister
.type fixtureCallsThroughARegister, @function
fixtureCallsThroughARegister:
)"
    FIXTURE_LANDING_PAD
    R"(    call *%rax
    nop
    nop
    nop
    ret
.size fixtureCallsThroughARegister, .-fixtureCallsThroughARegister
)");

// A symbol that is not a function, for the planner's type check.
//
// An ordinary definition rather than assembly like the fixtures above, because
// the compiler already emits exactly what this needs: an exported symbol of
// type STT_OBJECT with a size, which is what the planner reads. Writing it as a
// .data block instead cost an LTO build, where the debug line information GCC
// attaches to inline assembly has nowhere to go in a data section and the
// assembler rejects the result.
unsigned long long fixtureNotAFunction = 0x1122334455667788ULL;
