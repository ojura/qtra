// Binds a replacement through the host ABI and lets go of it again.
//
// The visualizer is the only other module that binds this way, and it needs
// PulseAudio, so on a machine without it nothing exercised the host path at
// all. This needs nothing beyond the host and the step function's own ABI, so
// it is always built and the mixed-stack behaviour can be tested anywhere.
//
// What it replaces the step function with is deliberately dull: a steady spin
// and a tint of its own, so a person watching can see it took effect. The point
// is which module the entry reaches and what the host says about it, not what
// the cube does.
//
// It does not call cube_step_builtin. Once the gateway is installed that name
// reaches the gateway, which reaches whatever is selected, which is this
// function: the call would come straight back and the stack would run out. A
// replacement that wants the original calls the address its binding handed
// back, which is past the gateway. This one wants nothing from it.
//
// {"release": true} lets go of the binding without unloading the module, so a
// test can release a binding that is not the selected one.

#include "agent/agent_abi.h"
#include "demo/cube_step_abi.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace {

// The host, kept whole. patch_unbind has to be called with the same context
// the binding was made under, and that context is valid for the process.
RuntimeAgentHost host{};
bool hostValid = false;
std::uint64_t binding = 0;

CubeStepOutput tintedStep(const CubeStepInput* input) noexcept
{
    CubeStepOutput output{};
    output.angle_degrees = input != nullptr
        ? input->angle_degrees + 40.0F * input->delta_seconds
        : 0.0F;
    output.tint_r = 0.4F;
    output.tint_g = 0.9F;
    output.tint_b = 0.6F;
    output.scale = 1.0F;
    return output;
}

void release(const RuntimeAgentHost* callingHost)
{
    if (callingHost == nullptr || callingHost->complete_json == nullptr) {
        return;
    }
    const bool had = binding != 0;
    if (had && hostValid) {
        // Released and not overwritten. Something bound after this one stays
        // selected, which a raw store would have destroyed.
        (void)host.patch_unbind(host.agent_context, binding);
        binding = 0;
    }
    callingHost->complete_json(
        callingHost->invocation_context,
        QJsonDocument(QJsonObject{{QStringLiteral("released"), had}})
            .toJson(QJsonDocument::Compact)
            .constData());
}

void run(const RuntimeAgentHost* callingHost)
{
    if (callingHost == nullptr || callingHost->complete_json == nullptr) {
        return;
    }

    const char* requestJson = callingHost->request_json != nullptr
        ? callingHost->request_json(callingHost->invocation_context)
        : nullptr;
    const QJsonObject request = QJsonDocument::fromJson(
        QByteArray(requestJson != nullptr ? requestJson : "{}")).object();
    if (request.value(QStringLiteral("release")).toBool(false)) {
        release(callingHost);
        return;
    }

    if (binding != 0) {
        callingHost->complete_json(
            callingHost->invocation_context,
            QJsonDocument(
                QJsonObject{{QStringLiteral("binding"), QString::number(binding)},
                            {QStringLiteral("alreadyBound"), true}})
                .toJson(QJsonDocument::Compact)
                .constData());
        return;
    }

    host = *callingHost;
    hostValid = true;

    RuntimeAgentPatchBinding bound{};
    // No flags, so this answers to whatever the build recorded about the
    // target and is refused where it recorded nothing.
    const std::int32_t result = host.patch_bind(host.agent_context,
                                                reinterpret_cast<void*>(&cube_step_builtin),
                                                reinterpret_cast<void*>(&tintedStep),
                                                0,
                                                &bound);
    if (result != 0) {
        callingHost->fail(callingHost->invocation_context,
                          QByteArray("the host refused to bind ("
                                     + QByteArray::number(result) + ")")
                              .constData());
        return;
    }

    binding = bound.id;
    callingHost->complete_json(
        callingHost->invocation_context,
        QJsonDocument(QJsonObject{{QStringLiteral("binding"), QString::number(binding)}})
            .toJson(QJsonDocument::Compact)
            .constData());
}

const RuntimeAgentSnippet descriptor{
    RUNTIME_AGENT_ABI,
    sizeof(RuntimeAgentSnippet),
    "bind a replacement through the host ABI",
    &run,
    &release,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippet*
runtime_agent_snippet_init()
{
    return &descriptor;
}
