#include "demo/cube_step_abi.h"

#include <cmath>

namespace {

CubeStepOutputV1 reverseStep(const CubeStepInputV1* input) noexcept
{
    if (input == nullptr) {
        return CubeStepOutputV1{0.0F, 0.4F, 0.8F, 1.0F, 1.0F};
    }
    float angle = input->angle_degrees
        - std::abs(input->angular_velocity_degrees_per_second) * input->delta_seconds;
    angle = std::fmod(angle, 360.0F);
    if (angle < 0.0F) {
        angle += 360.0F;
    }
    return CubeStepOutputV1{angle, 0.45F, 0.85F, 1.0F, 1.0F};
}

const CubeStepPatchV1 descriptor{
    CUBE_STEP_ABI_V1,
    sizeof(CubeStepPatchV1),
    "reverse/blue patch",
    &reverseStep,
};

} // namespace

extern "C" RUNTIME_AGENT_EXPORT const CubeStepPatchV1* cube_step_patch_init_v1()
{
    return &descriptor;
}
