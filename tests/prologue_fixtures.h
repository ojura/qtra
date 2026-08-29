#pragma once

// Functions whose opening bytes are known exactly, so a test can name the
// answer it expects from the planner and mean it.

#include <cstdint>

extern "C" {

// Adds one to its argument. Its prologue is whole instructions the decoder
// reads, none of which names an address as a distance, and nothing in its body
// branches back into them.
int fixtureAddsOne(int value);

// Reads a counter through an operand naming its address as a distance from the
// instruction after it, so the copy has to have that distance corrected.
int fixtureCounter(void);
extern int fixtureCounterStorage;

// Opens with a floating-point instruction, which this decoder does not read.
int fixtureUndecodable(void);

// Opens with a jump, which names where it goes as a distance from itself.
int fixtureOpensWithABranch(void);

// Loops back to a place inside the bytes a jump would be written over.
int fixtureBranchesIntoItsPrologue(void);

} // extern "C"
