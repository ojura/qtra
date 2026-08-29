// A snippet that reports what the host actually handed it.
//
// Three questions this answers, all of which used to have an answer that read
// as reasonable until somebody asked what consumed the value:
//
//   what a scalar request arrives as, which was silently replaced with {}
//   what a log carries, which omitted the module it came from
//   what patch_unbind returns, which was picked by matching error text
//
// It echoes rather than asserts. A snippet cannot fail a test on its own, so it
// completes with what it saw and the test outside decides whether that is
// right.

#include "agent/agent_abi.h"

#include <cstdio>
#include <string>

namespace {

// The request as JSON, quoted so the test can compare it exactly. A scalar has
// to survive as a scalar: 42 is not {} and not "42".
std::string quoted(const char* text)
{
    std::string out = "\"";
    for (const char* c = text; c != nullptr && *c != '\0'; ++c) {
        if (*c == '"' || *c == '\\') {
            out += '\\';
        }
        out += *c;
    }
    out += '"';
    return out;
}

void run(const RuntimeAgentHost* host)
{
    if (host == nullptr || host->complete_json == nullptr) {
        return;
    }

    // Logged first, so the event carrying it is in history by the time the test
    // asks. What matters is not the text but what the host attaches to it.
    if (host->log != nullptr) {
        host->log(host->agent_context, 0, "protocol probe reporting");
    }

    // Three unbind answers, each asked of a binding this module does not hold.
    //
    // An id nothing ever handed out is not live. Zero is the same case stated
    // differently, and asking twice for the same absent id has to give the same
    // answer rather than drifting.
    std::int32_t notLive = 0;
    std::int32_t alsoNotLive = 0;
    if (host->patch_unbind != nullptr) {
        notLive = host->patch_unbind(host->agent_context, 0xFFFFFFFFULL);
        alsoNotLive = host->patch_unbind(host->agent_context, 0);
    }

    const std::string request =
        host->request_json != nullptr
            ? std::string(host->request_json(host->invocation_context))
            : std::string("null");

    std::string result = "{";
    result += "\"sawRequest\":" + quoted(request.c_str());
    result += ",\"unbindUnknownId\":" + std::to_string(notLive);
    result += ",\"unbindZeroId\":" + std::to_string(alsoNotLive);
    result += "}";

    host->complete_json(host->invocation_context, result.c_str());
}

const RuntimeAgentSnippet descriptor{
    RUNTIME_AGENT_ABI,
    sizeof(RuntimeAgentSnippet),
    "reports what the host handed it",
    &run,
    nullptr,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippet* runtime_agent_snippet_init()
{
    return &descriptor;
}
