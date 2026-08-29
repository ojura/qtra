#include "agent/coverage_manifest.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>

namespace runtime_agent {

CoverageDecision readCoverageManifest(const QString& manifestPath,
                                      const QString& hostBuildId,
                                      const QString& target)
{
    CoverageDecision decision;

    QFile file(manifestPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        decision.reason = QStringLiteral(
            "no coverage manifest at %1, so nothing recorded whether replacing this "
            "function reaches every call. Build with PATCH_READY=ON, which keeps the "
            "split debug information and clone dumps that decision needs")
            .arg(manifestPath);
        return decision;
    }

    QJsonParseError parse{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        decision.reason = QStringLiteral("the coverage manifest at %1 could not be read: %2")
                              .arg(manifestPath, parse.errorString());
        return decision;
    }

    const QJsonObject report = document.object();
    decision.report = report;
    decision.coverage = report.value(QStringLiteral("coverage")).toString();
    decision.target = report.value(QStringLiteral("target")).toString();
    decision.manifestBuildId = report.value(QStringLiteral("buildId")).toString();

    // Identity first. A verdict about another build describes another binary's
    // entries and offsets, and agreeing with it would be worse than having none.
    if (decision.manifestBuildId.isEmpty() || hostBuildId.isEmpty()
        || decision.manifestBuildId != hostBuildId) {
        decision.allow = false;
        decision.reason = QStringLiteral(
            "the coverage manifest describes build %1 and this process is build %2, so "
            "its verdict is about a different binary")
            .arg(decision.manifestBuildId.isEmpty() ? QStringLiteral("(none)")
                                                    : decision.manifestBuildId,
                 hostBuildId.isEmpty() ? QStringLiteral("(none)") : hostBuildId);
        return decision;
    }

    if (decision.target != target) {
        decision.allow = false;
        decision.reason = QStringLiteral(
            "the coverage manifest decided about %1, and this is a request about %2")
            .arg(decision.target, target);
        return decision;
    }

    // Past this point the verdict is about this binary and this function, so
    // what it says about them can be read.
    decision.describesThisTarget = true;

    const QJsonObject domain =
        report.value(QStringLiteral("callerExecutionDomain")).toObject();
    decision.domainStrength = domain.value(QStringLiteral("strength")).toString();
    decision.authorizesRequestBoundary =
        domain.value(QStringLiteral("authorizesRequestBoundary")).toBool(false);

    if (decision.coverage != QStringLiteral("complete")) {
        decision.allow = false;
        const QJsonArray unknown = report.value(QStringLiteral("unknown")).toArray();
        QStringList names;
        for (const QJsonValue& value : unknown) {
            names.append(value.toString());
        }
        decision.reason = QStringLiteral("coverage is %1").arg(decision.coverage);
        if (!names.isEmpty()) {
            decision.reason += QStringLiteral(", unanswered: %1").arg(names.join(", "));
        }
        const QJsonArray skipped = report.value(QStringLiteral("skipped")).toArray();
        if (!skipped.isEmpty()) {
            decision.reason += QStringLiteral(
                ". %1 copy or copies of this function would keep running the original")
                .arg(skipped.size());
        }
        return decision;
    }

    decision.allow = true;
    decision.reason = QStringLiteral("coverage is complete for %1 in build %2")
                          .arg(decision.target, decision.manifestBuildId);
    return decision;
}

bool admitsEntryWrite(const CoverageDecision& decision,
                      const bool acceptIncompleteCoverage,
                      QString& error)
{
    if (!decision.describesThisTarget) {
        error = decision.reason;
        return false;
    }

    // The write happens because this thread is the only one that reaches the
    // target, and that claim comes from the manifest. A claim that was only
    // observed does not support it: having seen one thread arrive says nothing
    // about one that arrives under a condition nobody exercised.
    if (!decision.authorizesRequestBoundary) {
        error = QStringLiteral(
            "the recorded claim about which threads reach this function is %1, which "
            "does not support writing its entry while the process runs. Only a proved "
            "or declared claim does")
            .arg(decision.domainStrength.isEmpty() ? QStringLiteral("absent")
                                                   : decision.domainStrength);
        return false;
    }

    if (!decision.allow) {
        if (acceptIncompleteCoverage) {
            return true;
        }
        error = QStringLiteral("%1. Accept incomplete coverage to proceed anyway, which "
                               "installs a replacement that may not reach every caller")
                    .arg(decision.reason);
        return false;
    }
    return true;
}

} // namespace runtime_agent
