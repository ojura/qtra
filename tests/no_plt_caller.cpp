// A caller built without procedure linkage table stubs.
//
// With stubs, a call to another object lands on a stub which jumps through a
// slot, and the relocation that fills that slot is a jump slot. Without them
// the call goes straight through the slot, and the relocation sits with the
// object's ordinary data relocations instead. Both are slots a redirect can
// use, and they are in different tables, so a resolver that reads one table
// finds this object's calls invisible.
//
// Built with -fno-plt for exactly that reason. The test asks this object what
// it calls and expects to be told, which is what fails when only one table is
// read.

#include "no_plt_caller.h"

#include <cstdlib>
#include <cstring>

extern "C" {

std::size_t noPltCallerMeasures(const char* text)
{
    // Through a slot, because strlen is in another object and this was built
    // without a stub to reach it.
    return std::strlen(text);
}

void* noPltCallerAllocates(const std::size_t bytes)
{
    return std::malloc(bytes);
}

void noPltCallerFrees(void* memory)
{
    std::free(memory);
}

} // extern "C"
