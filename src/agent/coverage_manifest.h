#pragma once

// What the build decided about replacing a function, read back at runtime.
//
// Whether replacing a function reaches every call is settled while compiling
// and gone from the optimized binary afterwards. The build writes that decision
// down; this reads it and refuses to act on a decision about a different build.
//
// The check is identity, not trust. A manifest naming a build id other than the
// running one describes some other binary, and its verdict says nothing about
// the offsets and entries in this one.

#include <QHash>
#include <QJsonObject>
#include <QString>

namespace runtime_agent {

struct CoverageDecision {
    // Whether this verdict is about this build and this function. False for a
    // missing or unreadable manifest, a different build id, and a different
    // target. Separate from allow, because those are refusals no caller may
    // accept its way past: they mean nothing was decided about the entry that
    // is about to be written.
    bool describesThisTarget = false;

    // complete, incomplete or unknown, as the build found it. Empty when no
    // manifest was found at all, which is its own answer.
    QString coverage;

    // Whether activation may proceed on this evidence.
    bool allow = false;

    // Why, in terms a caller can act on.
    QString reason;

    // Whether the recorded claim about which threads reach the target is strong
    // enough to stand in for stopping execution. A claim that was only observed
    // is not: seeing one thread arrive does not rule out another.
    bool authorizesRequestBoundary = false;
    QString domainStrength;

    // What the manifest named, kept for reporting.
    QString target;
    QString manifestBuildId;
    QJsonObject report;

    [[nodiscard]] bool present() const noexcept { return !coverage.isEmpty(); }
};

// Reads the manifest beside a build and checks it describes this process.
//
// Refuses on anything it cannot establish: no manifest, unreadable, a different
// build id, a different target, or a verdict other than complete. Absence is a
// refusal because a build that recorded nothing has not been asked the
// question, and treating silence as approval is the failure this exists to
// prevent.
[[nodiscard]] CoverageDecision readCoverageManifest(const QString& manifestPath,
                                                    const QString& hostBuildId,
                                                    const QString& target);

// The build's decision answers two questions, and which of them a caller has
// to satisfy depends on what the caller is about to do.
//
// Both start from identity, and neither can be accepted past it. A manifest
// about another build or another function has decided nothing about the entry
// in front of the caller, and the flag below would otherwise wave through the
// case with the least evidence behind it.

// Whether this replacement may be the thing that runs.
//
// The question is coverage: whether replacing the function reaches every call.
// acceptIncompleteCoverage takes a replacement that some callers will not
// reach, which is the one thing that flag means. It applies to any selection,
// since a replacement that misses callers misses them however it was chosen.
[[nodiscard]] bool admitsReplacementEffect(const CoverageDecision& decision,
                                           bool acceptIncompleteCoverage,
                                           QString& error);

// Whether the target's entry bytes may be rewritten while the process runs.
//
// The question is the recorded claim about which threads reach the function,
// which is what stands in for stopping them. Nothing accepts past it: taking a
// replacement that misses callers is a wrong effect, and writing the entry
// underneath a thread that is executing it is not the same kind of thing.
//
// Asked only by operations that write bytes, which are installing the gateway
// and recovering from an install that could not finish. Once a gateway exists,
// choosing what runs is one aligned store into its slot: every call reads the
// old destination or the new one, both stay valid because nothing is ever
// unloaded, and no byte of the target changes. A store that reclaimed a
// generation's code or state would need its own proof that no call is still
// inside it, which is a different question from this one.
[[nodiscard]] bool authorizesLiveTextWrite(const CoverageDecision& decision,
                                           QString& error);

// The last verdict that described the running binary, kept for the process.
//
// Evidence about a process does not stop being true because the file it came
// from was replaced. Rebuilding the tree while this one runs leaves a manifest
// describing a different binary, and refusing on that would let an unrelated
// build revoke what this build established about itself.
//
// Process-lifetime for the same reason the patched entry's owner is: a gateway
// outlives whoever installed it, so a successor asking about the same target
// has to find what the first admission established, and losing it would leave a
// running gateway that nothing could bind through after a rebuild.
//
// Separate from what a recovery record holds, and for a different reason. This
// is re-asked on every request and answers what may run. That is never
// re-asked, and says what one write was made under.
class CoverageEvidence final {
public:
    // The one every adapter shares, allocated once and never destroyed.
    [[nodiscard]] static CoverageEvidence& instance();

    // Records a fresh reading and answers with what now stands for this target.
    //
    // A reading that names this build and this function replaces what is held,
    // whether it allows or refuses: that is the same binary speaking again, and
    // a reread that could only ever help would be a cache dressed as evidence.
    // A reading that names something else, or found nothing, leaves the held
    // one standing.
    [[nodiscard]] CoverageDecision refresh(const QString& target,
                                           const CoverageDecision& reading);

private:
    QHash<QString, CoverageDecision> m_held;
};

} // namespace runtime_agent
