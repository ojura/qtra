#include "demo/cube_step_abi.h"

#include <cmath>

namespace {

CubeStepOutputV1 wobbleStep(const CubeStepInputV1* input) noexcept
{
    if (input == nullptr) {
        return CubeStepOutputV1{0.0F, 1.0F, 0.2F, 0.2F, 1.0F};
    }

    float angle = input->angle_degrees
        + input->angular_velocity_degrees_per_second * input->delta_seconds * 0.55F;
    angle = std::fmod(angle, 360.0F);
    if (angle < 0.0F) {
        angle += 360.0F;
    }

    const float phase = input->elapsed_seconds;
    const float red = 0.55F + 0.45F * std::sin(phase * 1.7F);
    const float green = 0.55F + 0.45F * std::sin(phase * 1.7F + 2.094F);
    const float blue = 0.55F + 0.45F * std::sin(phase * 1.7F + 4.188F);
    const float scale = 0.88F + 0.15F * std::sin(phase * 2.4F);
    return CubeStepOutputV1{angle, red, green, blue, scale};
}

const CubeStepPatchV1 descriptor{
    CUBE_STEP_ABI_V1,
    sizeof(CubeStepPatchV1),
    "wobble/rainbow patch",
    &wobbleStep,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const CubeStepPatchV1* cube_step_patch_init_v1()
{
    return &descriptor;
}
