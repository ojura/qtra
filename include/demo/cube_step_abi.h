#pragma once

#include <cstdint>
#include "agent/agent_abi.h"

extern "C" {

inline constexpr std::uint32_t CUBE_STEP_ABI_V1 = 0x0001'0000u;

struct CubeStepInputV1 {
    float angle_degrees;
    float angular_velocity_degrees_per_second;
    float delta_seconds;
    float elapsed_seconds;
    std::uint64_t frame_index;
};

struct CubeStepOutputV1 {
    float angle_degrees;
    float tint_r;
    float tint_g;
    float tint_b;
    float scale;
};

using CubeStepFunctionV1 = CubeStepOutputV1 (*)(const CubeStepInputV1* input) noexcept;

struct CubeStepPatchV1 {
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    const char* name_utf8;
    CubeStepFunctionV1 step;
};

using CubeStepPatchInitV1 = const CubeStepPatchV1* (*)();

// The optimized host calls this through a function pointer. On GCC/x86-64 its
// entry contains a reserved NOP area, allowing the raw hotpatch demo to redirect
// it without relocating a real prologue.
RUNTIME_AGENT_EXPORT CubeStepOutputV1
cube_step_builtin_v1(const CubeStepInputV1* input) noexcept;

// Every cube patch module exports this exact symbol.
RUNTIME_AGENT_EXPORT const CubeStepPatchV1* cube_step_patch_init_v1();

} // extern "C"
