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

// Whether the entry may be written, given what the build decided.
//
// The order matters and the questions are not interchangeable. Whether the
// manifest is about this binary and this function, and whether the recorded
// claim about which threads reach it is strong enough to write while the
// process runs, are settled first and cannot be accepted past. Only then may a
// caller take a replacement that some callers will not reach, which is the one
// thing acceptIncompleteCoverage means.
//
// Putting the flag first would let it wave through a manifest for another
// build, which is the case with the least evidence behind it.
[[nodiscard]] bool admitsEntryWrite(const CoverageDecision& decision,
                                    bool acceptIncompleteCoverage,
                                    QString& error);

} // namespace runtime_agent
