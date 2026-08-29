#pragma once

#include <cstddef>

extern "C" {

// Calls strlen through a slot, with no stub in between.
std::size_t noPltCallerMeasures(const char* text);

// Calls malloc and free the same way, so a redirect on one object's allocator
// can be told apart from a redirect on another's.
void* noPltCallerAllocates(std::size_t bytes);
void noPltCallerFrees(void* memory);

} // extern "C"
