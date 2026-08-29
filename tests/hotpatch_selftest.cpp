#include "agent/entry_hotpatch.h"
#include "agent/patch_manager.h"
#include "agent/quiescence_providers.h"
#include "agent/patch_area.h"
#include "agent/patch_site.h"
#include "demo/cube_step_abi.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>

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
    auto init = reinterpret_cast<CubeStepPatchInit>(::dlsym(module, "cube_step_patch_init"));
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

    // Where the compiler says this function may be patched.
    runtime_agent::PatchSite site;
    std::string resolveError;
    if (!runtime_agent::resolvePatchSite(reinterpret_cast<void*>(&cube_step_builtin),
                                         runtime_agent::patchAreaBytes,
                                         site,
                                         resolveError)) {
        std::cerr << "resolving the recorded patch site failed: " << resolveError << '\n';
        return 7;
    }

    const auto* targetBytes = reinterpret_cast<const std::uint8_t*>(
        reinterpret_cast<void*>(&cube_step_builtin));
    const bool cetTarget = std::equal(endbr64.begin(), endbr64.end(), targetBytes);
    const auto expectedPatchAddress = reinterpret_cast<std::uintptr_t>(targetBytes)
        + (cetTarget ? endbr64.size() : 0U);
    if (reinterpret_cast<std::uintptr_t>(site.patchAddress) != expectedPatchAddress
        || site.availableBytes != runtime_agent::patchAreaBytes
        || site.requiresEndbr64 != cetTarget) {
        std::cerr << "the resolved site does not describe the prepared area\n";
        return 8;
    }

    runtime_agent::PatchSite unprepared;
    std::string refusal;
    if (runtime_agent::resolvePatchSite(reinterpret_cast<void*>(&approximatelyEqual),
                                        runtime_agent::patchAreaBytes,
                                        unprepared,
                                        refusal)
        || refusal.empty()) {
        std::cerr << "an unprepared function resolved to a patch site\n";
        return 9;
    }

    // An area holding anything but its reserved NOPs takes no gateway.
    std::array<std::uint8_t, 20> occupied{};
    occupied.fill(0xCCU);
    runtime_agent::PatchSite occupiedSite;
    occupiedSite.entry = occupied.data();
    occupiedSite.patchAddress = occupied.data();
    occupiedSite.availableBytes = occupied.size();
    refusal.clear();
    if (runtime_agent::siteAcceptsGateway(occupiedSite, refusal) || refusal.empty()) {
        std::cerr << "an occupied area accepted a gateway\n";
        return 10;
    }

    // Behind a CET landing pad the jump is indirect, so its destination has to
    // be one too.
    runtime_agent::PatchSite cetSite;
    cetSite.entry = occupied.data();
    cetSite.patchAddress = occupied.data();
    cetSite.availableBytes = occupied.size();
    cetSite.requiresEndbr64 = true;
    std::array<std::uint8_t, 4> notALandingPad{0x90U, 0x90U, 0x90U, 0x90U};
    refusal.clear();
    if (runtime_agent::replacementIsReachable(cetSite, notALandingPad.data(), refusal)
        || refusal.empty()) {
        std::cerr << "a CET site accepted a replacement without ENDBR64\n";
        return 11;
    }

    runtime_agent::SingleThreadQuiescer quiet;

    // An install that copies the gateway and cannot put the mapping back. The
    // slot already names the continuation at that point, so the original still
    // runs, and the saved bytes are the only way back to a pristine entry.
    {
        runtime_agent::PatchManager faulted;
        runtime_agent::PatchBinding binding;
        faulted.setProtectFunction(&failRestoreProtect);
        std::string faultError;
        if (faulted.bind(site, reinterpret_cast<void*>(patch->step), 1, quiet, binding, faultError)) {
            std::cerr << "activate reported success although the mapping was never restored\n";
            return 12;
        }
        if (faulted.state() != runtime_agent::PatchState::RecoveryRequired) {
            std::cerr << "a write that changed bytes did not leave recovery required\n";
            return 13;
        }
        const CubeStepOutput duringFault = cube_step_builtin(&input);
        if (!approximatelyEqual(duringFault.angle_degrees, before.angle_degrees)) {
            std::cerr << "a half-installed gateway did not still run the original\n";
            return 14;
        }

        faulted.setProtectFunction(nullptr);
        if (!faulted.recover(faultError)) {
            std::cerr << "recovery failed: " << faultError << '\n';
            return 15;
        }
        if (faulted.state() != runtime_agent::PatchState::NoGateway) {
            std::cerr << "recovery did not return the entry to its own bytes\n";
            return 16;
        }
    }

    // With a thread nobody can account for, refusing is the answer, and the
    // entry has to be exactly as it was afterwards. A caller acts on this by
    // installing earlier, so the refusal is worth more than a write would be.
    {
        std::atomic<bool> keepRunning{true};
        std::thread worker([&keepRunning] {
            while (keepRunning.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });

        std::array<std::uint8_t, runtime_agent::patchAreaBytes> beforeRefusal{};
        std::memcpy(beforeRefusal.data(), site.patchAddress, beforeRefusal.size());

        if (runtime_agent::observedThreadCount() < 2) {
            std::cerr << "the worker thread was not observed, so the refusal proves nothing\n";
            keepRunning.store(false, std::memory_order_release);
            worker.join();
            return 33;
        }

        runtime_agent::SingleThreadQuiescer counted;
        std::string countedError;
        if (counted.acquire(countedError) != nullptr || countedError.empty()) {
            std::cerr << "a second thread did not stop the single-thread policy\n";
            keepRunning.store(false, std::memory_order_release);
            worker.join();
            return 34;
        }

        runtime_agent::PatchManager refused;
        runtime_agent::RefusingQuiescer refusing;
        runtime_agent::PatchBinding unused;
        std::string refusedError;
        if (refused.bind(site, reinterpret_cast<void*>(patch->step), 1, refusing,
                         unused, refusedError)
            || refusedError.empty()) {
            std::cerr << "a refusing policy still bound a replacement\n";
            keepRunning.store(false, std::memory_order_release);
            worker.join();
            return 35;
        }
        if (refused.state() != runtime_agent::PatchState::NoGateway) {
            std::cerr << "a refused install left state behind\n";
            keepRunning.store(false, std::memory_order_release);
            worker.join();
            return 36;
        }

        std::array<std::uint8_t, runtime_agent::patchAreaBytes> afterRefusal{};
        std::memcpy(afterRefusal.data(), site.patchAddress, afterRefusal.size());
        keepRunning.store(false, std::memory_order_release);
        worker.join();
        if (beforeRefusal != afterRefusal) {
            std::cerr << "a refused install changed the entry\n";
            return 37;
        }
    }

    // The gateway proper.
    runtime_agent::PatchManager patches;
    runtime_agent::PatchBinding first;
    std::string error;
    if (!patches.bind(site, reinterpret_cast<void*>(patch->step), 1, quiet, first, error)) {
        std::cerr << "activate failed: " << error << '\n';
        return 17;
    }
    if (patches.state() != runtime_agent::PatchState::GatewayReplacement) {
        std::cerr << "activate did not select a replacement\n";
        return 18;
    }
    if (cetTarget && !std::equal(endbr64.begin(), endbr64.end(), targetBytes)) {
        std::cerr << "the CET landing pad was overwritten\n";
        return 19;
    }

    const CubeStepOutput during = cube_step_builtin(&input);
    if (approximatelyEqual(during.angle_degrees, before.angle_degrees)
        && approximatelyEqual(during.scale, before.scale)) {
        std::cerr << "the selected replacement did not run\n";
        return 20;
    }

    // The gateway stays. Rolling back is a store naming the continuation, and
    // the original runs again through an entry that is still rewritten.
    if (!patches.unbind(first.id, 1, error)) {
        std::cerr << "unbind failed: " << error << '\n';
        return 21;
    }
    if (patches.state() != runtime_agent::PatchState::GatewayOriginal) {
        std::cerr << "rollback did not leave the gateway naming its continuation\n";
        return 22;
    }
    const CubeStepOutput after = cube_step_builtin(&input);
    if (!approximatelyEqual(after.angle_degrees, before.angle_degrees)
        || !approximatelyEqual(after.scale, before.scale)) {
        std::cerr << "the continuation did not run the original function\n";
        return 23;
    }

    // Selecting again writes no code at all.
    runtime_agent::PatchBinding second;
    if (!patches.bind(site, reinterpret_cast<void*>(patch->step), 1, quiet, second, error)) {
        std::cerr << "rebinding failed: " << error << '\n';
        return 24;
    }
    if (second.original != first.original) {
        std::cerr << "the continuation moved between bindings\n";
        return 27;
    }
    const CubeStepOutput again = cube_step_builtin(&input);
    if (approximatelyEqual(again.angle_degrees, before.angle_degrees)) {
        std::cerr << "reselecting did not reach the replacement\n";
        return 25;
    }
    // A binding released out of order leaves the slot alone, because it is not
    // the one selected.
    runtime_agent::PatchBinding third;
    if (!patches.bind(site, reinterpret_cast<void*>(patch->step), 2, quiet, third, error)) {
        std::cerr << "third bind failed: " << error << '\n';
        return 26;
    }
    if (!patches.unbind(second.id, 1, error)) {
        std::cerr << "releasing a binding underneath the selected one failed: " << error << '\n';
        return 28;
    }
    if (patches.status().selectedBinding != third.id) {
        std::cerr << "releasing an unselected binding changed the selection\n";
        return 29;
    }
    std::string ownership;
    if (patches.unbind(third.id, 1, ownership) || ownership.empty()) {
        std::cerr << "a module released a binding it does not own\n";
        return 30;
    }
    if (!patches.unbind(third.id, 2, error)) {
        std::cerr << "final unbind failed: " << error << '\n';
        return 31;
    }
    if (patches.state() != runtime_agent::PatchState::GatewayOriginal) {
        std::cerr << "releasing the last binding did not return to the original\n";
        return 32;
    }

    std::cout << "PASS: " << patch->name_utf8
              << " selected and deselected through a permanent gateway, a failed"
                 " install left recoverable state, and a refused install with an"
                 " unaccounted thread changed nothing\n";
    // Intentionally do not dlclose: the running app follows the same policy.
    return 0;
#endif
}
