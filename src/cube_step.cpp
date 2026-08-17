#include "demo/cube_step_abi.h"

#include <cmath>

#if defined(__GNUC__) && defined(__x86_64__)
#  define DEMO_PATCHABLE_ENTRY \
    __attribute__((noinline, noclone, used, visibility("default"), \
                   patchable_function_entry(16, 0)))
#elif defined(__GNUC__)
#  define DEMO_PATCHABLE_ENTRY \
    __attribute__((noinline, noclone, used, visibility("default")))
#else
#  define DEMO_PATCHABLE_ENTRY
#endif

extern "C" DEMO_PATCHABLE_ENTRY CubeStepOutputV1
cube_step_builtin_v1(const CubeStepInputV1* input) noexcept
{
    if (input == nullptr) {
        return CubeStepOutputV1{0.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    }

    float angle = input->angle_degrees
        + input->angular_velocity_degrees_per_second * input->delta_seconds;
    angle = std::fmod(angle, 360.0F);
    if (angle < 0.0F) {
        angle += 360.0F;
    }

    return CubeStepOutputV1{angle, 1.0F, 1.0F, 1.0F, 1.0F};
}
