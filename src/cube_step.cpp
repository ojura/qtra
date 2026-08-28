#include "demo/cube_step_abi.h"

#include <cmath>

// Nothing here prepares this function for replacement. The entry pad, the
// opaque object boundary that keeps callers from reasoning about this body, and
// the identity rules are all set by the build; see the flags applied to this
// file in CMakeLists.txt. A target does not carry its patchability in source.
//
// The visibility that lets the agent resolve this by name comes from
// RUNTIME_AGENT_EXPORT on the declaration in cube_step_abi.h.
extern "C" CubeStepOutput cube_step_builtin(const CubeStepInput* input) noexcept
{
    if (input == nullptr) {
        return CubeStepOutput{0.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    }

    float angle = input->angle_degrees
        + input->angular_velocity_degrees_per_second * input->delta_seconds;
    angle = std::fmod(angle, 360.0F);
    if (angle < 0.0F) {
        angle += 360.0F;
    }

    return CubeStepOutput{angle, 1.0F, 1.0F, 1.0F, 1.0F};
}
