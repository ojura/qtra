// What a second adapter finds after the first one is gone.
//
// A patched entry keeps its generations for the life of the process, and a
// generation names the module that bound it. If the records of those modules
// died with whoever loaded them, a binding could stay selected while nothing
// could say whose it was or release it. That is the teardown case, and holding
// two registries and hoping is not a proof of it.
//
// The widget is null throughout. Loading a module, looking one up and reading
// what a patched entry holds never touch it, and constructing one here would
// need a display for something none of these questions involve.

#include "agent/module_manager.h"
#include "agent/module_registry.h"
#include "agent/patch_registry.h"

#include "demo/cube_step_abi.h"

#include <QCoreApplication>

#include <cstdio>
#include <memory>

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
        std::printf("usage: module_teardown_selftest <snippet module>\n");
        return 2;
    }
    const QString modulePath = QString::fromLocal8Bit(argv[1]);

    quint64 loadedId = 0;
    QString loadedName;

    std::printf("a module outlives the adapter that loaded it\n");
    {
        auto first = std::make_unique<ModuleManager>(nullptr);
        QString error;
        ModuleManager::LoadedModule* module = first->loadSnippet(modulePath, error);
        check(module != nullptr, qPrintable(QStringLiteral("loaded %1: %2")
                                                .arg(modulePath, error)));
        if (module == nullptr) {
            return 1;
        }
        loadedId = module->id;
        loadedName = module->name;

        check(first->module(loadedId) != nullptr, "the adapter that loaded it can find it");
        first.reset();

        // The adapter is gone. Anything that was only its knowledge is gone
        // with it, and this is the question that matters: was the module.
        auto second = std::make_unique<ModuleManager>(nullptr);
        ModuleManager::LoadedModule* survivor = second->module(loadedId);
        check(survivor != nullptr, "a later adapter finds the module the first one loaded");
        if (survivor != nullptr) {
            check(survivor->name == loadedName, "and it is the same module, by name");
            check(survivor->id == loadedId, "under the same id");
        }
        check(!second->list().isEmpty(), "and it lists it");
    }

    std::printf("ids are not handed out twice across adapters\n");
    {
        auto third = std::make_unique<ModuleManager>(nullptr);
        QString error;
        ModuleManager::LoadedModule* again = third->loadSnippet(modulePath, error);
        check(again != nullptr, "the same file loads again as a new module");
        if (again != nullptr) {
            check(again->id != loadedId,
                  "with an id the first one does not already have, so a generation naming "
                  "the first is not answered by the second");
        }
    }

    std::printf("the patched entry is the same one whoever asks\n");
    {
        auto* entry = reinterpret_cast<void*>(&cube_step_builtin);
        runtime_agent::PatchManager& viaRegistry =
            runtime_agent::PatchRegistry::instance().forEntry(entry);

        auto adapter = std::make_unique<ModuleManager>(nullptr);
        const QJsonObject before = adapter->patchStatus();
        adapter.reset();

        auto successor = std::make_unique<ModuleManager>(nullptr);
        const QJsonObject after = successor->patchStatus();

        check(before.value(QStringLiteral("entryState"))
                  == after.value(QStringLiteral("entryState")),
              "a successor reports the same entry state");
        check(&viaRegistry == &runtime_agent::PatchRegistry::instance().forEntry(entry),
              "and the same manager is behind both of them");
    }

    std::printf("%s\n", failures == 0 ? "all teardown checks passed" : "teardown checks failed");
    return failures == 0 ? 0 : 1;
}
