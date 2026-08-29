// A module that is still running when the agent is destroyed, and one that
// calls back after it.
//
// The host ABI says agent_context and the callbacks last for the life of the
// process, and sanctions keeping a copy of the host with invocation_context
// cleared so a module can log or emit from a callback later. Those two promises
// are what this exercises: a run that is still inside the module when the agent
// goes away, and a saved host used once it has.
//
// Not stamped with a host build id, for the same reason teardown_module is not:
// the loader here is a test binary rather than the application.

#include "agent/agent_abi.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

// Set once run() has been entered, so a test can destroy the agent at the
// moment module code is on a stack rather than hoping to hit the window.
std::atomic<bool> inside{false};

// Cleared by the test to let run() finish.
std::atomic<bool> held{true};

// What run() saw, kept so the test can ask afterwards.
std::atomic<bool> finished{false};

// The host, minus the invocation, which is what the ABI says may be kept.
RuntimeAgentHost saved{};
std::atomic<bool> savedIsSet{false};

void run(const RuntimeAgentHost* host)
{
    if (host == nullptr) {
        return;
    }
    saved = *host;
    saved.invocation_context = nullptr;
    savedIsSet.store(true, std::memory_order_release);

    inside.store(true, std::memory_order_release);
    while (held.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Still inside the module, so anything the host frees on the way out would
    // be freed under this.
    if (host->log != nullptr) {
        host->log(host->agent_context, 0, "lifetime module finishing");
    }
    finished.store(true, std::memory_order_release);

    if (host->complete_json != nullptr) {
        host->complete_json(host->invocation_context, "{}");
    }
}

const RuntimeAgentSnippet descriptor{
    RUNTIME_AGENT_ABI,
    sizeof(RuntimeAgentSnippet),
    "a module that runs until it is told to stop",
    &run,
    nullptr,
};

} // namespace

extern "C" {

RUNTIME_AGENT_EXPORT const RuntimeAgentSnippet* runtime_agent_snippet_init()
{
    return &descriptor;
}

// The handles the test drives this with.
RUNTIME_AGENT_EXPORT bool lifetimeModuleIsInside()
{
    return inside.load(std::memory_order_acquire);
}

RUNTIME_AGENT_EXPORT void lifetimeModuleRelease()
{
    held.store(false, std::memory_order_release);
}

RUNTIME_AGENT_EXPORT bool lifetimeModuleFinished()
{
    return finished.load(std::memory_order_acquire);
}

// Uses the saved host after the agent is gone, which the ABI sanctions and
// which must not reach freed storage.
RUNTIME_AGENT_EXPORT bool lifetimeModuleCallAfterwards()
{
    if (!savedIsSet.load(std::memory_order_acquire) || saved.log == nullptr) {
        return false;
    }
    saved.log(saved.agent_context, 0, "lifetime module calling after teardown");
    return true;
}

} // extern "C"
