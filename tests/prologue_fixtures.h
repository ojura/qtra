#pragma once

// Functions whose opening bytes are known exactly, so a test can name the
// answer it expects from the planner and mean it.

#include <cstdint>

extern "C" {

// Adds one to its argument. Its prologue is whole instructions the decoder
// reads, none of which names an address as a distance, and nothing in its body
// branches back into them.
int fixtureAddsOne(int value);

// Not a function. Declared so a test can take its address and ask the planner
// what it makes of a symbol whose bytes are data.
extern unsigned long long fixtureNotAFunction;

// Reads a counter through an operand naming its address as a distance from the
// instruction after it, so the copy has to have that distance corrected.
int fixtureCounter(void);
extern int fixtureCounterStorage;

// Opens with CPUID, which this decoder can step over but has not approved for
// relocation. Planned, never called.
int fixtureKnownButUnapproved(void);

// Opens with a floating-point instruction, which this decoder does not read.
int fixtureUndecodable(void);

// Opens with a jump, which names where it goes as a distance from itself.
int fixtureOpensWithABranch(void);

// Loops back to a place inside the bytes a jump would be written over.
int fixtureBranchesIntoItsPrologue(void);

// Puts a value in r11 in its opening bytes and reads it afterwards.
//
// r11 is caller-saved, so a gateway at a function's entry may use it freely:
// nothing has run yet. The jump back at the end of a relocated copy is not at
// an entry, and the instructions just copied are the function's own. One of
// them putting something in r11 that the rest of the function reads is exactly
// this, and a jump through r11 there returns a different answer with no crash
// to notice.
int fixtureKeepsAValueInR11(void);

// Calls through a register in its opening bytes. Planned, never called: the
// register holds nothing.
//
//
// A thread that has gone through that call is standing in the callee with its
// return address inside the bytes a jump would be written over. Nothing that
// reads instruction pointers sees it, and where the call goes is decided at run
// time, so scanning the body cannot rule it out either.
int fixtureCallsThroughARegister(void);

} // extern "C"
