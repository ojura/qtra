#include "agent/entry_hotpatch.h"
#include "demo/cube_step_abi.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <dlfcn.h>
#include <iostream>
#include <string>

#if defined(__linux__)
#  include <sys/mman.h>
#endif

namespace {

constexpr std::array<std::uint8_t, 4> endbr64{0xF3U, 0x0FU, 0x1EU, 0xFAU};

bool approximatelyEqual(float a, float b)
{
    return std::abs(a - b) < 0.0001F;
}

#if defined(__linux__) && defined(__x86_64__)
// Allows the mapping to be made writable and refuses to put it back, which is
// the order that leaves the bytes changed after the caller has been told the
// write failed.
int failRestoreProtect(void* address, std::size_t length, int protection)
{
    if ((protection & PROT_WRITE) != 0) {
        return ::mprotect(address, length, protection);
    }
    errno = EACCES;
    return -1;
}
#endif

} // namespace

int main(int argc, char** argv)
{
#if !defined(__linux__) || !defined(__x86_64__)
    std::cout << "SKIP: raw hotpatch self-test only runs on Linux/x86-64\n";
    return 0;
#else
    if (argc != 2) {
        std::cerr << "usage: hotpatch_selftest /absolute/path/to/patch.so\n";
        return 2;
    }

    void* module = ::dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr) {
        std::cerr << "dlopen failed: " << ::dlerror() << '\n';
        return 3;
    }

    auto init = reinterpret_cast<CubeStepPatchInit>(
        ::dlsym(module, "cube_step_patch_init"));
    if (init == nullptr) {
        std::cerr << "dlsym failed: " << ::dlerror() << '\n';
        return 4;
    }
    const CubeStepPatch* patch = init();
    if (patch == nullptr || patch->abi_version != CUBE_STEP_ABI || patch->step == nullptr) {
        std::cerr << "invalid patch descriptor\n";
        return 5;
    }

    const CubeStepInput input{10.0F, 90.0F, 0.5F, 1.25F, 77};
    const CubeStepOutput before = cube_step_builtin(&input);
    if (!approximatelyEqual(before.angle_degrees, 55.0F)
        || !approximatelyEqual(before.scale, 1.0F)) {
        std::cerr << "unexpected builtin result before patch\n";
        return 6;
    }

    // Refuse to overwrite a normal/unknown prologue. The demo patcher only
    // accepts the all-NOP area emitted by patchable_function_entry(16, 0).
    std::array<std::uint8_t, 16> invalidEntry{};
    invalidEntry.fill(0xCCU);
    runtime_agent::EntryHotpatch rejectedPatch;
    std::string rejectedError;
    if (rejectedPatch.apply(invalidEntry.data(),
                            reinterpret_cast<void*>(patch->step),
                            invalidEntry.size(),
                            rejectedError)
        || rejectedError.empty()) {
        std::cerr << "non-NOP entry was not rejected\n";
        return 7;
    }

    // A target prepared for Intel CET/IBT may only jump indirectly to another
    // valid ENDBR64 landing pad. This test uses data buffers so the rejection
    // path is exercised in every build, not only when -fcf-protection is on.
    std::array<std::uint8_t, 20> cetEntry{};
    std::copy(endbr64.begin(), endbr64.end(), cetEntry.begin());
    std::fill(cetEntry.begin() + static_cast<std::ptrdiff_t>(endbr64.size()),
              cetEntry.end(), 0x90U);
    std::array<std::uint8_t, 4> nonCetReplacement{0x90U, 0x90U, 0x90U, 0x90U};
    runtime_agent::EntryHotpatch rejectedCetPatch;
    rejectedError.clear();
    if (rejectedCetPatch.apply(cetEntry.data(),
                               nonCetReplacement.data(),
                               16,
                               rejectedError)
        || rejectedError.empty()) {
        std::cerr << "CET target accepted a replacement without ENDBR64\n";
        return 8;
    }

    runtime_agent::EntryHotpatch hotpatch;
    std::string error;
    if (!hotpatch.apply(reinterpret_cast<void*>(&cube_step_builtin),
                        reinterpret_cast<void*>(patch->step),
                        16,
                        error)) {
        std::cerr << "apply failed: " << error << '\n';
        return 9;
    }

    const auto* targetBytes = reinterpret_cast<const std::uint8_t*>(
        reinterpret_cast<void*>(&cube_step_builtin));
    const bool cetTarget = std::equal(endbr64.begin(), endbr64.end(), targetBytes);
    const auto expectedPatchAddress = reinterpret_cast<std::uintptr_t>(targetBytes)
        + (cetTarget ? endbr64.size() : 0U);
    if (reinterpret_cast<std::uintptr_t>(hotpatch.patchAddress()) != expectedPatchAddress
        || (cetTarget && !std::equal(endbr64.begin(), endbr64.end(), targetBytes))) {
        std::cerr << "patcher did not preserve/use the expected entry address\n";
        return 10;
    }

    const CubeStepOutput during = cube_step_builtin(&input);
    if (approximatelyEqual(during.angle_degrees, before.angle_degrees)
        && approximatelyEqual(during.scale, before.scale)) {
        std::cerr << "patched function still produced the builtin result\n";
        return 11;
    }

    if (!hotpatch.rollback(error)) {
        std::cerr << "rollback failed: " << error << '\n';
        return 12;
    }

    const CubeStepOutput after = cube_step_builtin(&input);
    if (!approximatelyEqual(after.angle_degrees, before.angle_degrees)
        || !approximatelyEqual(after.scale, before.scale)) {
        std::cerr << "rollback did not restore the builtin function\n";
        return 13;
    }

    // A permission restore that fails after the copy is the one path that
    // leaves the process changed while the caller is told the install failed.
    // A real mprotect will not fail on demand, so the call is substituted.
    {
        runtime_agent::EntryHotpatch faulted;
        faulted.setProtectFunction(&failRestoreProtect);
        std::string faultError;
        if (faulted.apply(reinterpret_cast<void*>(&cube_step_builtin),
                          reinterpret_cast<void*>(patch->step),
                          16,
                          faultError)) {
            std::cerr << "apply reported success although the mapping was never restored\n";
            return 14;
        }
        if (faulted.state() != runtime_agent::PatchState::RecoveryRequired) {
            std::cerr << "a write that changed bytes did not leave recovery required\n";
            return 15;
        }
        if (!faulted.active() || faulted.reservedBytes() == 0) {
            std::cerr << "the saved original bytes were discarded after a partial install\n";
            return 16;
        }

        const CubeStepOutput faultedOutput = cube_step_builtin(&input);
        if (approximatelyEqual(faultedOutput.angle_degrees, before.angle_degrees)
            && approximatelyEqual(faultedOutput.scale, before.scale)) {
            std::cerr << "the entry was reported rewritten but still ran the builtin\n";
            return 17;
        }

        // Recovery has to be possible from the state the failure left behind.
        faulted.setProtectFunction(nullptr);
        if (!faulted.rollback(faultError)) {
            std::cerr << "rollback after a partial install failed: " << faultError << '\n';
            return 18;
        }
        const CubeStepOutput recovered = cube_step_builtin(&input);
        if (!approximatelyEqual(recovered.angle_degrees, before.angle_degrees)
            || !approximatelyEqual(recovered.scale, before.scale)) {
            std::cerr << "recovery did not restore the builtin function\n";
            return 19;
        }
    }

    std::cout << "PASS: " << patch->name_utf8
              << " redirected and restored cube_step_builtin, and a failed"
                 " permission restore left recoverable state\n";
    // Intentionally do not dlclose: the running app follows the same policy.
    return 0;
#endif
}
