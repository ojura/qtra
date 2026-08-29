// A replacement in a module the loader places, for the distance it is placed at.
//
// The point of this file is where its code ends up. A module is mapped wherever
// the loader chooses, which on this system is terabytes from the executable, so
// a replacement here cannot be reached by a jump that names its destination as
// a thirty-two bit distance. That is the case the relocated-prologue backend
// exists to serve, and the one it could not serve while its entry jump named a
// destination instead of reading one.

extern "C" {

int farReplacementCalls = 0;

// Matches fixtureAddsOne's signature and answers differently, so a test can
// tell which of them ran.
int farAddsOne(int value)
{
    ++farReplacementCalls;
    return value + 1000;
}

// Matches fixtureCounter's signature. Used where a test drives the install
// through PatchManager, so the answer tells apart the replacement, the copy of
// the original, and a chained replacement calling what it displaced.
int farCounter(void)
{
    ++farReplacementCalls;
    return 4200;
}

// Calls whatever it is handed, which is what a replacement bound over an
// earlier one has to be able to do.
int (*farChainTo)(void) = nullptr;

int farCounterChained(void)
{
    ++farReplacementCalls;
    return farChainTo != nullptr ? farChainTo() + 100 : -1;
}

} // extern "C"
