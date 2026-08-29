#include "agent/entry_hotpatch.h"
#include "agent/got_site.h"
#include "agent/patch_registry.h"
#include "agent/prologue_relocation.h"
#include "tests/prologue_fixtures.h"
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
#include <chrono>
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

int replacementMemcpyCalls = 0;
char gotScratch[64] = {};

// What the fixtures reach once their prologues have been moved. Distinct
// answers, so a test cannot pass by the original having run.
extern "C" int replacementAddsOne(int value)
{
    return value * 101;
}

extern "C" int replacementCounter()
{
    return -1;
}

// Copies without calling memcpy, which would come straight back through the
// slot this replaces.
extern "C" void* countingMemcpy(void* destination, const void* source, std::size_t count)
{
    ++replacementMemcpyCalls;
    auto* to = static_cast<unsigned char*>(destination);
    const auto* from = static_cast<const unsigned char*>(source);
    for (std::size_t i = 0; i < count; ++i) {
        to[i] = from[i];
    }
    return destination;
}

// Out of line, with both the length and the destination read through volatiles
// so the compiler can see neither.
//
// Two things stop this reaching memcpy otherwise, and both cost a version of
// this test that reported the redirect had not worked. A constant length lets
// the copy be expanded where it stands. A destination whose size is visible
// lets _FORTIFY_SOURCE call __memcpy_chk instead, which is a different symbol
// with a different slot.
__attribute__((noinline)) void copyThroughTheTable(char* to, const char* from,
                                                   const std::size_t count)
{
    volatile std::size_t opaqueCount = count;
    char* volatile opaqueDestination = to;
    std::memcpy(opaqueDestination, from, opaqueCount);
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

// Fails the restore on its first write and behaves normally afterwards, so one
// manager can be driven into recovery and then back out of it: a manager is
// given its writer once, and recovery is itself a write. The count belongs to
// the writer, so another one made here starts again from the beginning.
class FailFirstRestoreWriter final : public runtime_agent::TextWriter {
public:
    [[nodiscard]] runtime_agent::TextWriteResult write(void* address,
                                                       const std::uint8_t* bytes,
                                                       const std::size_t size) override
    {
        if (m_pending) {
            m_pending = false;
            return runtime_agent::writeText(address, bytes, size, &failRestoreProtect);
        }
        return m_mapped.write(address, bytes, size);
    }

private:
    bool m_pending = true;
    runtime_agent::MappedTextWriter m_mapped;
};
#endif

// The two operations composed, which is what a caller wanting "make this run"
// does. Stated here so every use below reads the same and the admission is
// written down once.
//
// AlreadyQuiescent is the truth here: this runs before anything else in the
// process can reach the target, and no manifest is involved at all. A
// manifest-shaped admission would be a claim the self-test has no evidence for.
bool installAndBind(runtime_agent::PatchManager& manager,
                    const runtime_agent::PatchSite& site,
                    void* replacement,
                    const std::uint64_t owner,
                    runtime_agent::Quiescer& quiescer,
                    runtime_agent::PatchBinding& binding,
                    std::string& error)
{
    // Checked before anything is written, so a replacement that will be
    // refused does not leave a permanent gateway behind. bind checks again
    // against the site the gateway was installed at.
    if (!replacementIsReachable(site, replacement, error)) {
        return false;
    }

    if (manager.state() == runtime_agent::PatchState::NoGateway) {
        const runtime_agent::LiveTextWriteAdmission admission(
            runtime_agent::WriteAdmissionBasis::AlreadyQuiescent,
            quiescer.name(),
            site.name.empty() ? std::string("cube_step_builtin") : site.name,
            "the self-test writes before anything can reach the target");
        if (!manager.installGateway(site, admission, quiescer, error)) {
            return false;
        }
    }
    return manager.bind(replacement, owner, binding, error);
}

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

    // The loader knows this object by its file name, which is what the GOT
    // lookup below matches by suffix.
    const std::string patchModulePath = argv[1];
    const std::string patchModuleName =
        patchModulePath.substr(patchModulePath.find_last_of('/') + 1);

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

    // A call into a library this build does not produce, redirected without a
    // prepared area and without writing anyone's code.
    //
    // This is the case the entry patcher cannot serve. Reserving space at a
    // function's entry means building the object that defines it, and libc is
    // not ours to build. The relocation is, because it belongs to the object
    // doing the calling.
    {
        std::string gotError;
        runtime_agent::GotSite site;
        if (!runtime_agent::resolveGotSlot("", "memcpy", site, gotError)) {
            std::cerr << "could not find the slot memcpy is called through: " << gotError << '\n';
            return 40;
        }
        if (site.resolved != ::dlsym(RTLD_DEFAULT, "memcpy")) {
            std::cerr << "the slot does not hold what the loader resolved\n";
            return 41;
        }

        const int before = replacementMemcpyCalls;
        copyThroughTheTable(gotScratch, "unredirected", 13);
        if (replacementMemcpyCalls != before) {
            std::cerr << "the replacement ran before anything was redirected\n";
            return 42;
        }

        if (!runtime_agent::redirectGotSlot(
                site, reinterpret_cast<void*>(&countingMemcpy), gotError)) {
            std::cerr << "could not redirect the slot: " << gotError << '\n';
            return 43;
        }
        copyThroughTheTable(gotScratch, "redirected", 11);
        if (replacementMemcpyCalls != before + 1) {
            std::cerr << "the redirect did not reach the caller\n";
            return 44;
        }
        if (std::strcmp(gotScratch, "redirected") != 0) {
            std::cerr << "the replacement did not copy what it was asked to\n";
            return 45;
        }

        if (!runtime_agent::restoreGotSlot(site, gotError)) {
            std::cerr << "could not put the slot back: " << gotError << '\n';
            return 46;
        }
        if (*site.slot != site.resolved) {
            std::cerr << "restoring did not put back what the loader resolved\n";
            return 47;
        }
        const int after = replacementMemcpyCalls;
        copyThroughTheTable(gotScratch, "restored", 9);
        if (replacementMemcpyCalls != after) {
            std::cerr << "the replacement still ran after the slot was restored\n";
            return 48;
        }

        // The permissions the loader chose come back, whatever they were.
        //
        // This executable is bound eagerly, so its table is read-only once the
        // loader has finished. A module bound lazily keeps its table writable
        // because the loader still has symbols to resolve into it, and handing
        // that back read-only would fault the next call it tried to resolve.
        // The patch module under test is the second kind.
        {
            runtime_agent::GotSite inModule;
            std::string moduleError;
            if (runtime_agent::resolveGotSlot(patchModuleName, "fmodf", inModule,
                                              moduleError)) {
                const int chosen = inModule.pageProtection;
                if (!runtime_agent::redirectGotSlot(
                        inModule, reinterpret_cast<void*>(&countingMemcpy), moduleError)
                    || !runtime_agent::restoreGotSlot(inModule, moduleError)) {
                    std::cerr << "the module's slot could not be redirected and put back: "
                              << moduleError << '\n';
                    return 50;
                }
                int nowIs = 0;
                if (!runtime_agent::pageProtectionOf(inModule.slot, nowIs, moduleError)
                    || nowIs != chosen) {
                    std::cerr << "the round trip changed the permissions the loader chose\n";
                    return 51;
                }
            }
        }

        // Naming something the caller never calls has to say so, not guess.
        runtime_agent::GotSite absent;
        gotError.clear();
        if (runtime_agent::resolveGotSlot("", "no_such_function_anywhere", absent, gotError)
            || gotError.empty()) {
            std::cerr << "a symbol nobody calls resolved anyway\n";
            return 49;
        }
    }

    runtime_agent::SingleThreadQuiescer quiet;

    // A function with no space reserved at its entry, redirected by moving the
    // instructions that were there.
    //
    // The entry patcher needs an area the compiler set aside, so it reaches
    // only what this build prepares. A call through the table needs the call to
    // cross an object boundary, so it reaches only what is called from outside.
    // A function called from inside its own object, built without the flag, is
    // reachable by neither, and these fixtures are that.
    {
        std::string why;
        runtime_agent::ProloguePlan plan;
        const auto writer = runtime_agent::processTextWriter();

        // Each refusal, checked before anything that writes, because a refusal
        // arriving after the bytes changed would be no refusal at all.
        if (runtime_agent::planPrologueRelocation(
                reinterpret_cast<void*>(&fixtureUndecodable), plan, why)
            || why.find("not one this decoder reads") == std::string::npos) {
            std::cerr << "a prologue opening with an instruction the decoder cannot read "
                         "was planned anyway: " << why << '\n';
            return 50;
        }

        why.clear();
        if (runtime_agent::planPrologueRelocation(
                reinterpret_cast<void*>(&fixtureOpensWithABranch), plan, why)
            || why.find("branches somewhere named as a distance") == std::string::npos) {
            std::cerr << "a prologue opening with a branch was planned anyway: " << why << '\n';
            return 51;
        }

        why.clear();
        if (runtime_agent::planPrologueRelocation(
                reinterpret_cast<void*>(&fixtureBranchesIntoItsPrologue), plan, why)
            || why.find("inside the") == std::string::npos) {
            std::cerr << "a function looping back into its own opening bytes was planned "
                         "anyway: " << why << '\n';
            return 52;
        }

        // A function that leaves something in r11 among the bytes being moved
        // still returns it.
        //
        // r11 is caller-saved, so a gateway at an entry may use it. The jump
        // back at the end of a copy is not at an entry: the instructions just
        // copied are the function's own, and one of them putting a value in
        // r11 that the rest of the function reads is this. A jump through r11
        // there returns a different number with nothing crashing.
        {
            const int expected = fixtureKeepsAValueInR11();
            runtime_agent::ProloguePlan r11Plan;
            std::string r11Why;
            // Required to plan, not tried. This fixture is written so it can be,
            // so a refusal here is a regression in the planner and not a
            // reason to skip the check the fixture exists for.
            if (!runtime_agent::planPrologueRelocation(
                    reinterpret_cast<void*>(&fixtureKeepsAValueInR11), r11Plan, r11Why)) {
                std::cerr << "the r11 fixture could not be planned: " << r11Why << '\n';
                return 70;
            }
            {
                runtime_agent::RelocatedPrologue moved;
                runtime_agent::SingleThreadQuiescer alone;
                if (!runtime_agent::installRelocatedPrologue(
                        r11Plan, reinterpret_cast<void*>(&replacementAddsOne), alone,
                        *runtime_agent::processTextWriter(), moved, r11Why)) {
                    std::cerr << "the r11 fixture could not be relocated: " << r11Why << '\n';
                    return 71;
                }
                const auto original =
                    reinterpret_cast<int (*)()>(moved.original);
                const int throughCopy = original();
                if (!runtime_agent::restoreRelocatedPrologue(
                        moved, alone, *runtime_agent::processTextWriter(), r11Why)) {
                    std::cerr << "the r11 fixture could not be put back: " << r11Why << '\n';
                    return 72;
                }
                if (throughCopy != expected) {
                    std::cerr << "a value the function left in r11 did not survive the jump "
                                 "back from the copy: expected " << expected << ", got "
                              << throughCopy << '\n';
                    return 73;
                }
                if (fixtureKeepsAValueInR11() != expected) {
                    std::cerr << "the r11 fixture does not do what it did before\n";
                    return 74;
                }
            }
        }

        // A call through a register in the opening bytes has to be refused.
        {
            runtime_agent::ProloguePlan callPlan;
            std::string callWhy;
            if (runtime_agent::planPrologueRelocation(
                    reinterpret_cast<void*>(&fixtureCallsThroughARegister), callPlan,
                    callWhy)) {
                std::cerr << "a prologue calling through a register was accepted\n";
                return 75;
            }
            if (callWhy.find("hands control somewhere else") == std::string::npos) {
                std::cerr << "refused for the wrong reason: " << callWhy << '\n';
                return 76;
            }
        }

        // A plan that succeeds, and what it says about the site.
        why.clear();
        if (!runtime_agent::planPrologueRelocation(
                reinterpret_cast<void*>(&fixtureAddsOne), plan, why)) {
            std::cerr << "a plain prologue could not be planned: " << why << '\n';
            return 53;
        }
        if (plan.takenBytes < 5) {
            std::cerr << "the plan takes fewer bytes than a jump needs\n";
            return 54;
        }
        if (!plan.keepsLandingPad
            || plan.patchAddress != static_cast<std::uint8_t*>(plan.entry) + 4) {
            std::cerr << "the plan would write over the landing pad, leaving every indirect "
                         "call to this function faulting\n";
            return 55;
        }

        if (fixtureAddsOne(10) != 11) {
            std::cerr << "the fixture does not do what it says before anything is moved\n";
            return 56;
        }

        runtime_agent::RelocatedPrologue moved;
        if (!runtime_agent::installRelocatedPrologue(
                plan, reinterpret_cast<void*>(&replacementAddsOne), quiet, *writer, moved,
                why)) {
            std::cerr << "moving a plain prologue failed: " << why << '\n';
            return 57;
        }
        if (fixtureAddsOne(10) != 1010) {
            std::cerr << "the redirect did not reach the replacement\n";
            return 58;
        }

        // The copy runs the instructions that were at the entry and then
        // continues into the rest of the body, so calling it is calling what
        // the function did before.
        const auto original = reinterpret_cast<int (*)(int)>(moved.original);
        if (original(10) != 11) {
            std::cerr << "the copy of the prologue does not run the original behaviour\n";
            return 59;
        }

        if (!runtime_agent::restoreRelocatedPrologue(moved, quiet, *writer, why)) {
            std::cerr << "putting the prologue back failed: " << why << '\n';
            return 60;
        }
        if (fixtureAddsOne(10) != 11) {
            std::cerr << "restoring did not put the original bytes back\n";
            return 61;
        }

        // An operand naming its address as a distance has to name the same
        // address once moved. The copy sits somewhere else entirely, so an
        // uncorrected distance would read whatever happens to be near it.
        why.clear();
        runtime_agent::ProloguePlan counterPlan;
        if (!runtime_agent::planPrologueRelocation(
                reinterpret_cast<void*>(&fixtureCounter), counterPlan, why)) {
            std::cerr << "a prologue reading through a distance could not be planned: "
                      << why << '\n';
            return 62;
        }
        if (!counterPlan.adjustedRipRelative) {
            std::cerr << "the plan did not notice the operand that names its address as a "
                         "distance, so this test proves nothing\n";
            return 63;
        }

        fixtureCounterStorage = 41;
        if (fixtureCounter() != 42) {
            std::cerr << "the counter fixture does not do what it says\n";
            return 64;
        }

        runtime_agent::RelocatedPrologue movedCounter;
        if (!runtime_agent::installRelocatedPrologue(
                counterPlan, reinterpret_cast<void*>(&replacementCounter), quiet, *writer,
                movedCounter, why)) {
            std::cerr << "moving a prologue that reads through a distance failed: " << why
                      << '\n';
            return 65;
        }
        const auto originalCounter = reinterpret_cast<int (*)()>(movedCounter.original);
        fixtureCounterStorage = 100;
        if (originalCounter() != 101) {
            std::cerr << "the copy read the wrong place, so the distance was not corrected "
                         "for where the copy sits\n";
            return 66;
        }
        if (!runtime_agent::restoreRelocatedPrologue(movedCounter, quiet, *writer, why)) {
            std::cerr << "putting the counter's prologue back failed: " << why << '\n';
            return 67;
        }
        fixtureCounterStorage = 41;
        if (fixtureCounter() != 42) {
            std::cerr << "restoring the counter did not put the original bytes back\n";
            return 68;
        }
    }

    // A replacement that cannot be reached must leave the entry as it was. The
    // gateway is permanent, so installing one for a request that then fails
    // would mean a refusal had written into the process's text.
    {
        runtime_agent::PatchRegistry registry;
        runtime_agent::PatchManager& refusedTarget = registry.forEntry(site.entry);
        runtime_agent::PatchBinding unused;
        std::string reachError;

        if (installAndBind(refusedTarget, site, nullptr, 1, quiet, unused, reachError)) {
            std::cerr << "a null replacement was accepted\n";
            return 30;
        }
        if (refusedTarget.state() != runtime_agent::PatchState::NoGateway) {
            std::cerr << "a refused replacement installed a gateway anyway\n";
            return 31;
        }

        // Only under CET is a destination without a landing pad unreachable.
        // Where the site does not require one, any address is a valid target
        // and there is nothing here to refuse.
        if (site.requiresEndbr64) {
            std::array<std::uint8_t, 8> notALandingPad{};
            notALandingPad.fill(0x90U);
            reachError.clear();
            if (installAndBind(refusedTarget, site, notALandingPad.data(), 1, quiet, unused,
                               reachError)) {
                std::cerr << "a destination without a landing pad was accepted\n";
                return 32;
            }
            if (refusedTarget.state() != runtime_agent::PatchState::NoGateway) {
                std::cerr << "a destination without a landing pad installed a gateway\n";
                return 33;
            }
        }

        // Nothing was written, so the function still is what it was.
        const CubeStepOutput untouched = cube_step_builtin(&input);
        if (!approximatelyEqual(untouched.angle_degrees, before.angle_degrees)) {
            std::cerr << "a refused replacement changed what the function does\n";
            return 34;
        }
    }

    // An install that copies the gateway and cannot put the mapping back. The
    // slot already names the continuation at that point, so the original still
    // runs, and the saved bytes are the only way back to a pristine entry.
    {
        runtime_agent::PatchRegistry registry(std::make_shared<FailFirstRestoreWriter>());
        runtime_agent::PatchManager& faulted = registry.forEntry(site.entry);
        runtime_agent::PatchBinding binding;
        std::string faultError;
        if (installAndBind(faulted, site, reinterpret_cast<void*>(patch->step), 1, quiet, binding, faultError)) {
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

        // What a later caller finds. The registry owns the manager, so asking
        // for this entry again reaches the same one, still holding the saved
        // bytes and the lease the failed write left. Whoever asked first is
        // irrelevant to that, which is the point: recovery does not depend on
        // the caller that caused the damage still being around.
        runtime_agent::PatchManager& successor = registry.forEntry(site.entry);
        if (&successor != &faulted) {
            std::cerr << "asking for one entry twice gave two managers\n";
            return 15;
        }
        if (successor.state() != runtime_agent::PatchState::RecoveryRequired) {
            std::cerr << "the registry did not keep what the failed write left\n";
            return 16;
        }
        // The record carries what admitted the write that made it, so a
        // successor can say what the entry was rewritten under without asking
        // anything that may have changed since.
        const runtime_agent::PatchStatus recovering = successor.status();
        if (!recovering.recoveryAdmission.has_value()) {
            std::cerr << "recovery state exists with nothing saying what admitted the write\n";
            return 22;
        }
        if (recovering.recoveryAdmission->basis()
                != runtime_agent::WriteAdmissionBasis::AlreadyQuiescent
            || recovering.recoveryAdmission->provider() != std::string(quiet.name())) {
            std::cerr << "the admission kept with the write is not the one it was made under\n";
            return 23;
        }
        if (successor.status().slotAddress != faulted.status().slotAddress) {
            std::cerr << "the gateway slot moved between two asks for one entry\n";
            return 17;
        }

        // A different entry is a different manager, so one site's trouble is
        // not another's.
        int elsewhere = 0;
        runtime_agent::PatchManager& other = registry.forEntry(&elsewhere);
        if (&other == &faulted) {
            std::cerr << "two entries shared one manager\n";
            return 18;
        }
        if (!registry.knows(site.entry) || registry.entries().size() != 2) {
            std::cerr << "the registry lost track of what it made\n";
            return 19;
        }

        // Recovering through the second reference proves it is the same state
        // and not a copy that merely reports the same thing.
        if (!successor.recover(faultError)) {
            std::cerr << "recovery failed: " << faultError << '\n';
            return 20;
        }
        if (faulted.state() != runtime_agent::PatchState::NoGateway) {
            std::cerr << "recovery did not return the entry to its own bytes\n";
            return 21;
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

        const auto observed = runtime_agent::observedThreadCount();
        if (!observed.has_value() || *observed < 2) {
            std::cerr << "the worker thread was not observed, so the refusal proves nothing\n";
            keepRunning.store(false, std::memory_order_release);
            worker.join();
            return 33;
        }

        runtime_agent::SingleThreadQuiescer counted;
        std::string countedError;
        if (counted.acquire(runtime_agent::WriteRegion{site.patchAddress, 20}, countedError) != nullptr || countedError.empty()) {
            std::cerr << "a second thread did not stop the single-thread policy\n";
            keepRunning.store(false, std::memory_order_release);
            worker.join();
            return 34;
        }

        runtime_agent::PatchManager refused;
        runtime_agent::RefusingQuiescer refusing;
        runtime_agent::PatchBinding unused;
        std::string refusedError;
        if (installAndBind(refused, site, reinterpret_cast<void*>(patch->step), 1, refusing,
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

        // Joining means the thread has finished, and its /proc/self/task entry
        // can outlive that by a moment. Everything below counts threads, so it
        // waits for the count to settle instead of racing the kernel tearing
        // the entry down.
        bool settled = false;
        for (int attempt = 0; attempt < 200 && !settled; ++attempt) {
            const auto now = runtime_agent::observedThreadCount();
            settled = now.has_value() && *now == 1;
            if (!settled) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        if (!settled) {
            std::cerr << "the worker's task entry did not go away, so a thread count "
                         "cannot be trusted here\n";
            return 38;
        }
    }

    // The gateway proper.
    runtime_agent::PatchManager patches;
    runtime_agent::PatchBinding first;
    std::string error;
    if (!installAndBind(patches, site, reinterpret_cast<void*>(patch->step), 1, quiet, first, error)) {
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
    if (!installAndBind(patches, site, reinterpret_cast<void*>(patch->step), 1, quiet, second, error)) {
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
    if (!installAndBind(patches, site, reinterpret_cast<void*>(patch->step), 2, quiet, third, error)) {
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
