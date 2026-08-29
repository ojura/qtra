// What a write that copies everything and cannot restore the permission leaves.
//
// Its own process, because it installs a gateway and gateways are permanent.
// There is one prepared entry here, so a test that consumes it cannot share a
// process with tests that need that entry as the compiler left it.
//
// The write has one outcome that changes bytes without finishing: the whole
// gateway is copied and the mapping cannot be made executable again. Nothing
// copies part of an instruction stream. So the entry holds a complete, working
// gateway, and what is wrong is a page that should not be writable, which is
// reported and needs no code write and nothing stopped to repair.

#include "agent/entry_hotpatch.h"
#include "agent/patch_area.h"
#include "agent/patch_registry.h"
#include "agent/patch_site.h"
#include "agent/quiescence_providers.h"
#include "demo/cube_step_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <errno.h>
#include <sys/mman.h>

namespace {

int failures = 0;

void check(const bool condition, const char* what)
{
    if (!condition) {
        std::printf("  FAIL %s\n", what);
        ++failures;
        return;
    }
    std::printf("  ok   %s\n", what);
}

bool approximatelyEqual(const float a, const float b)
{
    return std::fabs(a - b) < 0.0001F;
}

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

class FailRestoreWriter final : public runtime_agent::TextWriter {
public:
    [[nodiscard]] runtime_agent::TextWriteResult write(void* address,
                                                       const std::uint8_t* bytes,
                                                       const std::size_t count) override
    {
        return runtime_agent::writeText(address, bytes, count, &failRestoreProtect);
    }
};

CubeStepOutput replacementStep(const CubeStepInput* input) noexcept
{
    CubeStepOutput output{};
    output.angle_degrees = input != nullptr ? input->angle_degrees + 90.0F : 0.0F;
    output.tint_r = 1.0F;
    output.scale = 1.0F;
    return output;
}

// A policy that makes the site unusable for exactly as long as it holds
// execution.
//
// Installing checks the site twice: once with everything parked, in the form
// that answers without allocating, and again afterwards to say why when the
// first said no. Between those two the lease is released, so the site can
// become usable again in between, and then the second check has nothing to
// report. That is the state this stages, by scribbling on the prepared area
// while the lease exists and putting it back when the lease goes.
class BreaksTheSiteWhileParked : public runtime_agent::Quiescer {
public:
    class Lease : public runtime_agent::QuiescenceLease {
    public:
        explicit Lease(std::uint8_t* const area) : m_area(area)
        {
            // Not a NOP, so the site refuses.
            const std::uint8_t breaking = 0xCCU;
            (void)runtime_agent::writeText(m_area, &breaking, 1, nullptr);
        }
        ~Lease() override
        {
            const std::uint8_t nop = 0x90U;
            (void)runtime_agent::writeText(m_area, &nop, 1, nullptr);
        }

    private:
        std::uint8_t* m_area;
    };

    explicit BreaksTheSiteWhileParked(void* const area)
        : m_area(static_cast<std::uint8_t*>(area))
    {
    }

    std::unique_ptr<runtime_agent::QuiescenceLease> acquire(const runtime_agent::WriteRegion&,
                                                            std::string& error) override
    {
        error.clear();
        return std::make_unique<Lease>(m_area);
    }

    [[nodiscard]] const char* name() const noexcept override
    {
        return "breaks-the-site-while-parked";
    }

private:
    std::uint8_t* m_area;
};

} // namespace

int main()
{
    std::printf("a gateway written onto a page whose permission could not be put back\n");

    runtime_agent::PatchSite site;
    std::string why;
    if (!runtime_agent::resolvePatchSite(reinterpret_cast<void*>(&cube_step_builtin),
                                         runtime_agent::patchAreaBytes, site, why)) {
        std::printf("  skip  this build prepared no area: %s\n", why.c_str());
        return 0;
    }

    CubeStepInput input{};
    input.angle_degrees = 10.0F;
    input.delta_seconds = 0.016F;
    input.angular_velocity_degrees_per_second = 45.0F;
    const CubeStepOutput before = cube_step_builtin(&input);

    runtime_agent::PatchRegistry registry(std::make_shared<FailRestoreWriter>());
    runtime_agent::PatchManager& faulted = registry.forEntry(site.entry);
    runtime_agent::SingleThreadQuiescer quiet;

    const runtime_agent::LiveTextWriteAdmission admission(
        runtime_agent::WriteAdmissionBasis::AlreadyQuiescent,
        quiet.name(),
        site.name.empty() ? std::string("cube_step_builtin") : site.name,
        "this test is the only thread there is");

    std::string error;

    // First, the refusal that has nothing lasting to report.
    //
    // This runs before the install below because it writes nothing: the site is
    // usable again by the time it returns, and the entry is as the compiler left
    // it, which is what the rest of this needs.
    {
        BreaksTheSiteWhileParked awkward(site.patchAddress);
        const runtime_agent::LiveTextWriteAdmission staged(
            runtime_agent::WriteAdmissionBasis::AlreadyQuiescent,
            awkward.name(),
            site.name.empty() ? std::string("cube_step_builtin") : site.name,
            "this test is the only thread there is");
        std::string staging;
        check(!faulted.installGateway(site, staged, awkward, staging),
              "a site that stops being usable only while parked is refused");
        check(!staging.empty(), "and the refusal says something rather than nothing");
        check(staging.find("usable again now") != std::string::npos,
              staging.empty() ? "and says the reason did not last" : staging.c_str());
        check(runtime_agent::siteAcceptsGateway(site),
              "with the prepared area left as it was found");
    }

    check(faulted.installGateway(site, admission, quiet, error),
          error.empty() ? "the install reports success, because the gateway is complete"
                        : error.c_str());
    check(error.find("still writable") != std::string::npos,
          error.empty() ? "and says the page was left writable" : error.c_str());

    const runtime_agent::PatchStatus installed = faulted.status();
    check(installed.mappingLeftWritable, "which the status reports");
    check(installed.state == runtime_agent::PatchState::GatewayOriginal,
          "with the entry holding a gateway that names its continuation");
    check(installed.installedUnder.has_value()
              && installed.installedUnder->provider() == std::string(quiet.name()),
          "and what admitted the write kept with it");

    // The original still runs, because the slot names the continuation.
    const CubeStepOutput throughGateway = cube_step_builtin(&input);
    check(approximatelyEqual(throughGateway.angle_degrees, before.angle_degrees),
          "the original still runs through it");

    // And it works as a gateway: selecting a replacement is a store.
    runtime_agent::PatchBinding binding;
    check(faulted.bind(reinterpret_cast<void*>(&replacementStep), 1, binding, error),
          error.empty() ? "a replacement can be selected through it" : error.c_str());
    const CubeStepOutput replaced = cube_step_builtin(&input);
    check(!approximatelyEqual(replaced.angle_degrees, before.angle_degrees),
          "and is what runs");

    check(faulted.unbind(binding.id, 1, error),
          error.empty() ? "releasing it works" : error.c_str());
    const CubeStepOutput restored = cube_step_builtin(&input);
    check(approximatelyEqual(restored.angle_degrees, before.angle_degrees),
          "and puts the original back");

    // A later caller finds the same entry, still saying the same thing about
    // its page.
    runtime_agent::PatchManager& successor = registry.forEntry(site.entry);
    check(&successor == &faulted, "asking for this entry again gives the same manager");
    check(successor.status().mappingLeftWritable
              && successor.status().slotAddress == installed.slotAddress,
          "reporting the same page and the same slot");

    std::printf("%s\n", failures == 0
        ? "a complete gateway on a writable page is an installed gateway"
        : "it was not treated as one");
    return failures == 0 ? 0 : 1;
}
