#pragma once

#include <cstdint>
#include "agent/agent_abi.h"

extern "C" {

inline constexpr std::uint32_t CUBE_STEP_ABI = 0x0001'0000u;

struct CubeStepInput {
    float angle_degrees;
    float angular_velocity_degrees_per_second;
    float delta_seconds;
    float elapsed_seconds;
    std::uint64_t frame_index;
};

struct CubeStepOutput {
    float angle_degrees;
    float tint_r;
    float tint_g;
    float tint_b;
    float scale;
};

using CubeStepFunction = CubeStepOutput (*)(const CubeStepInput* input) noexcept;

struct CubeStepPatch {
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    const char* name_utf8;
    CubeStepFunction step;
};

using CubeStepPatchInit = const CubeStepPatch* (*)();

// The optimized host calls this through a function pointer. On GCC/x86-64 its
// entry contains a reserved NOP area, allowing the raw hotpatch demo to redirect
// it without relocating a real prologue.
RUNTIME_AGENT_EXPORT CubeStepOutput
cube_step_builtin(const CubeStepInput* input) noexcept;

// Every cube patch module exports this exact symbol.
RUNTIME_AGENT_EXPORT const CubeStepPatch* cube_step_patch_init();

} // extern "C"
