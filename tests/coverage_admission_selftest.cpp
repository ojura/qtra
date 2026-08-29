// The order the admission questions are asked in.
//
// Three questions decide whether the entry may be written, and they are not
// interchangeable. Whether the manifest is about this binary and this function,
// and whether the recorded claim about which threads reach it supports writing
// while the process runs, are settled before acceptIncompleteCoverage is looked
// at. That flag means one thing: the caller will take a replacement that some
// callers do not reach.
//
// These read manifests written into a temporary directory, so no build artifact
// is touched and being killed leaves nothing behind. readCoverageManifest and
// admitsEntryWrite need nothing but the file, which is why they are separate
// from ModuleManager.

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

// Whether the entry may be written given this manifest, asked both ways.
struct Answer {
    bool strict = false;
    bool accepting = false;
    QString strictError;
    QString acceptingError;
};

Answer ask(const QString& manifestPath)
{
    const runtime_agent::CoverageDecision decision =
        runtime_agent::readCoverageManifest(manifestPath, kBuildId, kTarget);
    Answer answer;
    answer.strict = runtime_agent::admitsEntryWrite(decision, false, answer.strictError);
    answer.accepting = runtime_agent::admitsEntryWrite(decision, true, answer.acceptingError);
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
        check(answer.strict, "admitted without the flag");
        check(answer.accepting, "admitted with it");
    }

    std::printf("no manifest at all\n");
    {
        const Answer answer = ask(directory.filePath(QStringLiteral("absent.json")));
        check(!answer.strict, "refused");
        check(!answer.accepting, "the flag does not accept it");
        check(answer.acceptingError.contains(QStringLiteral("no coverage manifest")),
              "and says nothing recorded the decision");
    }

    std::printf("a manifest that is not readable as JSON\n");
    {
        const QString path = directory.filePath(QStringLiteral("torn.json"));
        QFile file(path);
        check(file.open(QIODevice::WriteOnly), "wrote a torn file");
        file.write("{ this is not json");
        file.close();
        const Answer answer = ask(path);
        check(!answer.strict, "refused");
        check(!answer.accepting, "the flag does not accept it");
    }

    std::printf("a manifest about another build\n");
    {
        QJsonObject report = admissible();
        report.insert(QStringLiteral("buildId"), QStringLiteral("0000000000000000"));
        const Answer answer = ask(write(directory, QStringLiteral("other-build.json"), report));
        check(!answer.strict, "refused");
        check(!answer.accepting, "the flag does not accept it");
        check(answer.acceptingError.contains(QStringLiteral("different binary")),
              "and says whose verdict it is");
    }

    std::printf("a manifest about another function\n");
    {
        QJsonObject report = admissible();
        report.insert(QStringLiteral("target"), QStringLiteral("some_other_function"));
        const Answer answer = ask(write(directory, QStringLiteral("other-target.json"), report));
        check(!answer.strict, "refused");
        check(!answer.accepting, "the flag does not accept it");
        check(answer.acceptingError.contains(QStringLiteral("some_other_function")),
              "and names what it decided about");
    }

    std::printf("a claim about which threads reach the target that was only observed\n");
    {
        const QJsonObject report = withDomain(admissible(), QStringLiteral("observed"), false);
        const Answer answer = ask(write(directory, QStringLiteral("observed.json"), report));
        check(!answer.strict, "refused with complete coverage");
        check(!answer.accepting, "the flag does not accept it");
        check(answer.acceptingError.contains(QStringLiteral("which threads reach this function")),
              "and says which question it failed");
    }

    // The combination that decides the order. Coverage is incomplete, so the
    // flag has something to accept, and the domain claim does not support the
    // write. Asking the flag first admits this; asking the domain first refuses.
    std::printf("an unauthorized domain where the flag has something to accept\n");
    {
        const QJsonObject report = withCoverage(
            withDomain(admissible(), QStringLiteral("observed"), false),
            QStringLiteral("incomplete"));
        const Answer answer =
            ask(write(directory, QStringLiteral("observed-incomplete.json"), report));
        check(!answer.strict, "refused");
        check(!answer.accepting, "the flag does not reach past the domain question");
        check(answer.acceptingError.contains(QStringLiteral("which threads reach this function")),
              "and the domain is what it names, not the coverage");
    }

    std::printf("incomplete coverage, which is the one thing the flag means\n");
    {
        const QJsonObject report = withCoverage(admissible(), QStringLiteral("incomplete"));
        const Answer answer = ask(write(directory, QStringLiteral("incomplete.json"), report));
        check(!answer.strict, "refused without the flag");
        check(answer.strictError.contains(QStringLiteral("coverage is incomplete")),
              "and says what is missing");
        check(answer.accepting, "admitted with it");
    }

    std::printf("coverage the build could not decide\n");
    {
        const QJsonObject report = withCoverage(admissible(), QStringLiteral("unknown"));
        const Answer answer = ask(write(directory, QStringLiteral("unknown.json"), report));
        check(!answer.strict, "refused without the flag");
        check(answer.accepting, "admitted with it, the same as incomplete");
    }

    std::printf("%s\n", failures == 0 ? "all admission-order checks passed"
                                      : "admission-order checks failed");
    return failures == 0 ? 0 : 1;
}
