// An object whose call to malloc is still unresolved.
//
// The call goes through a procedure-linkage entry, which the loader fills in the
// first time it is taken. Nothing here calls it, so the slot still holds the
// resolver stub while other objects in this process have theirs bound.
//
// That mix is what a redirect across several callers has to refuse. Redirecting
// the bound slots alone leaves this one reaching the original as soon as the
// resolver runs, and nothing afterwards reports it.

#include <cstddef>
#include <cstdlib>

extern "C" {

// Reached by a call, so this one goes through the lazy entry. Never called from
// the test, which is what keeps it unresolved.
void* mixedSlotCallsMalloc(const std::size_t bytes)
{
    return std::malloc(bytes);
}

} // extern "C"
