// A module that does nothing, for the test that asks whether a module outlives
// the adapter that loaded it.
//
// Deliberately not stamped with a host build id. Stamping ties a module to one
// executable, and this one is loaded by a test binary that is not the
// application, so a stamp would refuse it for exactly the right reason and
// answer a question this test is not asking.

#include "agent/agent_abi.h"

namespace {

void run(const RuntimeAgentHost* host)
{
    if (host != nullptr && host->complete_json != nullptr) {
        host->complete_json(host->invocation_context, "{}");
    }
}

const RuntimeAgentSnippet descriptor{
    RUNTIME_AGENT_ABI,
    sizeof(RuntimeAgentSnippet),
    "a module that does nothing",
    &run,
    nullptr,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippet*
runtime_agent_snippet_init()
{
    return &descriptor;
}
