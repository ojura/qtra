// What the build's decision answers, and to whom.
//
// It answers two questions. Whether this replacement may be the thing that runs
// is about coverage, and acceptIncompleteCoverage takes a replacement that some
// callers do not reach. Whether the entry's bytes may be rewritten while the
// process runs is about the recorded claim on which threads reach the function,
// and nothing accepts past it. Both refuse first on a manifest that is about
// another build or another function, which the flag cannot reach either.
//
// The two come apart: a manifest with complete coverage and a domain that was
// only observed admits the replacement and refuses the write.
//
// These read manifests written into a temporary directory, so no build artifact
// is touched and being killed leaves nothing behind. Neither function needs
// anything but the file, which is why they are separate from ModuleManager.

#include "agent/coverage_manifest.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>

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

const QString kBuildId = QStringLiteral("abcdef0123456789");
const QString kTarget = QStringLiteral("cube_step_builtin");

// A manifest that would be admitted, for the cases to spoil one field at a time.
QJsonObject admissible()
{
    QJsonObject domain;
    domain.insert(QStringLiteral("strength"), QStringLiteral("declared"));
    domain.insert(QStringLiteral("authorizesRequestBoundary"), true);

    QJsonObject report;
    report.insert(QStringLiteral("buildId"), kBuildId);
    report.insert(QStringLiteral("target"), kTarget);
    report.insert(QStringLiteral("coverage"), QStringLiteral("complete"));
    report.insert(QStringLiteral("callerExecutionDomain"), domain);
    return report;
}

QString write(const QDir& directory, const QString& name, const QJsonObject& report)
{
    const QString path = directory.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        std::printf("  FAIL could not write %s\n", qPrintable(path));
        ++failures;
        return path;
    }
    file.write(QJsonDocument(report).toJson());
    return path;
}

// Both questions asked of one manifest, the first of them both ways.
struct Answer {
    bool effect = false;
    bool effectAccepting = false;
    bool write = false;
    QString effectError;
    QString effectAcceptingError;
    QString writeError;
};

Answer ask(const QString& manifestPath)
{
    const runtime_agent::CoverageDecision decision =
        runtime_agent::readCoverageManifest(manifestPath, kBuildId, kTarget);
    Answer answer;
    answer.effect =
        runtime_agent::admitsReplacementEffect(decision, false, answer.effectError);
    answer.effectAccepting =
        runtime_agent::admitsReplacementEffect(decision, true, answer.effectAcceptingError);
    answer.write = runtime_agent::authorizesLiveTextWrite(decision, answer.writeError);
    return answer;
}

QJsonObject withDomain(QJsonObject report, const QString& strength, const bool authorizes)
{
    QJsonObject domain;
    domain.insert(QStringLiteral("strength"), strength);
    domain.insert(QStringLiteral("authorizesRequestBoundary"), authorizes);
    report.insert(QStringLiteral("callerExecutionDomain"), domain);
    return report;
}

QJsonObject withCoverage(QJsonObject report, const QString& coverage)
{
    report.insert(QStringLiteral("coverage"), coverage);
    return report;
}

} // namespace

int main()
{
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        std::printf("could not make a temporary directory\n");
        return 1;
    }
    const QDir directory(temporary.path());

    std::printf("a manifest that answers every question\n");
    {
        const Answer answer = ask(write(directory, QStringLiteral("good.json"), admissible()));
        check(answer.effect, "the replacement is admitted without the flag");
        check(answer.effectAccepting, "and with it");
        check(answer.write, "and the entry may be written");
    }

    std::printf("no manifest at all\n");
    {
        const Answer answer = ask(directory.filePath(QStringLiteral("absent.json")));
        check(!answer.effect && !answer.effectAccepting, "no replacement is admitted");
        check(!answer.write, "and no write is authorized");
        check(answer.effectAcceptingError.contains(QStringLiteral("no coverage manifest")),
              "and it says nothing recorded the decision");
    }

    std::printf("a manifest that is not readable as JSON\n");
    {
        const QString path = directory.filePath(QStringLiteral("torn.json"));
        QFile file(path);
        check(file.open(QIODevice::WriteOnly), "wrote a torn file");
        file.write("{ this is not json");
        file.close();
        const Answer answer = ask(path);
        check(!answer.effect && !answer.effectAccepting, "no replacement is admitted");
        check(!answer.write, "and no write is authorized");
    }

    std::printf("a manifest about another build\n");
    {
        QJsonObject report = admissible();
        report.insert(QStringLiteral("buildId"), QStringLiteral("0000000000000000"));
        const Answer answer = ask(write(directory, QStringLiteral("other-build.json"), report));
        check(!answer.effect && !answer.effectAccepting, "no replacement is admitted");
        check(!answer.write, "and no write is authorized");
        check(answer.effectAcceptingError.contains(QStringLiteral("different binary"))
                  && answer.writeError.contains(QStringLiteral("different binary")),
              "and both say whose verdict it is");
    }

    std::printf("a manifest about another function\n");
    {
        QJsonObject report = admissible();
        report.insert(QStringLiteral("target"), QStringLiteral("some_other_function"));
        const Answer answer = ask(write(directory, QStringLiteral("other-target.json"), report));
        check(!answer.effect && !answer.effectAccepting, "no replacement is admitted");
        check(!answer.write, "and no write is authorized");
        check(answer.effectAcceptingError.contains(QStringLiteral("some_other_function")),
              "and it names what it decided about");
    }

    std::printf("a domain claim that was only observed, with coverage complete\n");
    {
        const QJsonObject report = withDomain(admissible(), QStringLiteral("observed"), false);
        const Answer answer = ask(write(directory, QStringLiteral("observed.json"), report));
        check(!answer.write, "the entry may not be written");
        check(answer.writeError.contains(QStringLiteral("which threads reach this function")),
              "and it says which question failed");
        // Coverage is complete, so there is nothing wrong with the replacement
        // itself. On a process whose gateway is already installed, choosing it
        // writes no bytes and this is the whole question.
        check(answer.effect, "and the replacement is still admitted");
    }

    // Both questions fail, each for its own reason. The flag reaches the
    // coverage one and cannot reach the other, which is the whole distinction
    // between them.
    std::printf("an unauthorized domain and incomplete coverage together\n");
    {
        const QJsonObject report = withCoverage(
            withDomain(admissible(), QStringLiteral("observed"), false),
            QStringLiteral("incomplete"));
        const Answer answer =
            ask(write(directory, QStringLiteral("observed-incomplete.json"), report));
        check(!answer.write, "the entry may not be written");
        check(answer.writeError.contains(QStringLiteral("which threads reach this function")),
              "and the domain is what it names, not the coverage");
        check(!answer.effect, "the replacement is refused without the flag");
        check(answer.effectAccepting, "and the flag accepts the coverage, which is all it means");
    }

    std::printf("incomplete coverage, which is the one thing the flag means\n");
    {
        const QJsonObject report = withCoverage(admissible(), QStringLiteral("incomplete"));
        const Answer answer = ask(write(directory, QStringLiteral("incomplete.json"), report));
        check(!answer.effect, "the replacement is refused without the flag");
        check(answer.effectError.contains(QStringLiteral("coverage is incomplete")),
              "and it says what is missing");
        check(answer.effectAccepting, "admitted with it");
        check(answer.write, "and coverage never decided whether the entry may be written");
    }

    std::printf("coverage the build could not decide\n");
    {
        const QJsonObject report = withCoverage(admissible(), QStringLiteral("unknown"));
        const Answer answer = ask(write(directory, QStringLiteral("unknown.json"), report));
        check(!answer.effect, "the replacement is refused without the flag");
        check(answer.effectAccepting, "admitted with it, the same as incomplete");
    }

    // What answers a later request once a manifest has been read. These write
    // into the temporary directory too, so no build artifact is touched and
    // being killed leaves nothing behind.
    std::printf("evidence held for a target, refreshed by readings about it\n");
    {
        runtime_agent::CoverageEvidence& evidence = runtime_agent::CoverageEvidence::instance();
        evidence.forget(kTarget);

        const auto read = [&](const QString& path) {
            return runtime_agent::readCoverageManifest(path, kBuildId, kTarget);
        };

        const runtime_agent::CoverageDecision good =
            evidence.refresh(kTarget, read(write(directory, QStringLiteral("held.json"),
                                                 admissible())));
        check(good.allow, "a reading about this build is what stands");

        // A rebuild leaves a manifest describing some other binary. That says
        // nothing about this one, so what was established stays established.
        QJsonObject elsewhere = admissible();
        elsewhere.insert(QStringLiteral("buildId"), QStringLiteral("0000000000000000"));
        const runtime_agent::CoverageDecision afterRebuild =
            evidence.refresh(kTarget, read(write(directory, QStringLiteral("rebuilt.json"),
                                                 elsewhere)));
        check(afterRebuild.allow, "a manifest about another build does not revoke it");

        const runtime_agent::CoverageDecision afterMissing =
            evidence.refresh(kTarget, read(directory.filePath(QStringLiteral("gone.json"))));
        check(afterMissing.allow, "and neither does a missing one");

        // A rerun against this build is the same binary speaking again, in
        // whichever direction it goes.
        const runtime_agent::CoverageDecision withdrawn =
            evidence.refresh(kTarget, read(write(directory, QStringLiteral("withdrawn.json"),
                                                 withCoverage(admissible(),
                                                              QStringLiteral("incomplete")))));
        check(!withdrawn.allow, "a rerun about this build can withdraw its verdict");

        const runtime_agent::CoverageDecision restored =
            evidence.refresh(kTarget, read(write(directory, QStringLiteral("restored.json"),
                                                 admissible())));
        check(restored.allow, "and can put it back");

        // Held per target, so one function's verdict is not another's.
        const runtime_agent::CoverageDecision other = evidence.refresh(
            QStringLiteral("some_other_function"),
            read(directory.filePath(QStringLiteral("gone.json"))));
        check(!other.allow, "another target holds nothing of this one's");

        evidence.forget(kTarget);
    }

    std::printf("%s\n", failures == 0 ? "all admission checks passed"
                                      : "admission checks failed");
    return failures == 0 ? 0 : 1;
}
