// What stamped says when the host has no build id to compare against.
//
// stamped means two build ids were compared and agreed. A module carrying an id
// loaded into a host that reports none establishes no such agreement, so it
// loads and stays unstamped, with the id it reported kept as metadata nobody
// verified.
//
// Its own binary because that is the only way to arrange the case: the host id
// is read from the running executable's ELF note, so this one is linked without
// one. Every other test binary carries a note and can never reach this path.

#include "agent/build_id.h"
#include "agent/module_registry.h"

#include <QCoreApplication>
#include <QJsonObject>

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

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc < 2) {
        std::printf("usage: unstamped_host_selftest <a module carrying a build id>\n");
        return 2;
    }

    std::printf("a module with a build id, in a host without one\n");

    check(runtime_agent::hostBuildId().isEmpty(),
          "this binary reports no build id, which is what makes the case reachable");

    QString error;
    runtime_agent::ModuleRegistry::LoadedModule* module =
        runtime_agent::ModuleRegistry::instance().loadSnippet(
            QString::fromLocal8Bit(argv[1]), error);
    check(module != nullptr,
          module != nullptr ? "the module loads" : qPrintable(error));
    if (module == nullptr) {
        return 1;
    }

    check(!module->targetBuildId.isEmpty(),
          "it reports the build it was compiled against");
    check(!module->stamped,
          "and is not stamped, because nothing here could agree with that id");

    const QJsonObject described = runtime_agent::ModuleRegistry::describe(*module);
    check(!described.value(QStringLiteral("stamped")).toBool(),
          "which is what the wire says too");

    std::printf("%s\n", failures == 0 ? "all unstamped-host checks passed"
                                      : "unstamped-host checks failed");
    return failures == 0 ? 0 : 1;
}
