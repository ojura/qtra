#include "demo/cube_step_abi.h"

#include <cmath>

namespace {

CubeStepOutput reverseStep(const CubeStepInput* input) noexcept
{
    if (input == nullptr) {
        return CubeStepOutput{0.0F, 0.4F, 0.8F, 1.0F, 1.0F};
    }
    float angle = input->angle_degrees
        - std::abs(input->angular_velocity_degrees_per_second) * input->delta_seconds;
    angle = std::fmod(angle, 360.0F);
    if (angle < 0.0F) {
        angle += 360.0F;
    }
    return CubeStepOutput{angle, 0.45F, 0.85F, 1.0F, 1.0F};
}

const CubeStepPatch descriptor{
    CUBE_STEP_ABI,
    sizeof(CubeStepPatch),
    "reverse/blue patch",
    &reverseStep,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const CubeStepPatch* cube_step_patch_init()
{
    return &descriptor;
}
