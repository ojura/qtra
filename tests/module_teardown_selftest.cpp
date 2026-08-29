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
#include "agent/patch_area.h"
#include "agent/patch_site.h"
#include "agent/quiescence_providers.h"

#include "demo/cube_step_abi.h"

#include <QCoreApplication>
#include <QJsonObject>

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

    runtime_agent::ModuleRegistry& registry = runtime_agent::ModuleRegistry::instance();

    std::printf("a module remains visible across adapter turnover\n");
    {
        auto first = std::make_unique<ModuleManager>(nullptr);
        QString error;
        runtime_agent::ModuleRegistry::LoadedModule* module = registry.loadSnippet(modulePath, error);
        check(module != nullptr, qPrintable(QStringLiteral("loaded %1: %2")
                                                .arg(modulePath, error)));
        if (module == nullptr) {
            return 1;
        }
        loadedId = module->id;
        loadedName = module->name;

        check(first->module(loadedId) != nullptr,
              "the first adapter can find the registry's module");
        first.reset();

        // The adapter is gone. The registry is process-lifetime, and a successor
        // must see the same module record rather than a private copy.
        auto second = std::make_unique<ModuleManager>(nullptr);
        runtime_agent::ModuleRegistry::LoadedModule* survivor = second->module(loadedId);
        check(survivor != nullptr, "a later adapter finds the same registry module");
        if (survivor != nullptr) {
            check(survivor->name == loadedName, "and it is the same module, by name");
            check(survivor->id == loadedId, "under the same id");
        }
        check(!registry.list().isEmpty(), "and the process registry lists it");
    }

    std::printf("what stamped claims\n");
    {
        // stamped says a module's build id was compared with this process's and
        // agreed. It is not "the module mentioned a build", and it is not "no
        // objection was raised": a module reporting an id into a host that
        // reports none establishes no agreement, and neither does the reverse.
        //
        // The module this test loads carries no id, so the value here is false
        // and the id is empty. What holds whatever a build produces is the
        // implication: stamped only where an id is present, since an agreement
        // with nothing to compare against cannot have happened.
        runtime_agent::ModuleRegistry::LoadedModule* record = registry.module(loadedId);
        check(record != nullptr, "the module is still registered");
        if (record != nullptr) {
            check(!record->stamped || !record->targetBuildId.isEmpty(),
                  "stamped is claimed only where the module reports a build id");
            if (record->targetBuildId.isEmpty()) {
                check(!record->stamped,
                      "and a module reporting no build id is not stamped, whatever the "
                      "host reports");
            }
            const QJsonObject described = registry.describe(*record);
            check(described.value(QStringLiteral("stamped")).toBool() == record->stamped,
                  "with the wire saying the same as the record");
        }
    }

    std::printf("one loaded object has one identity\n");
    {
        // The loader keys objects by pathname and hands back the same handle for
        // the same file, refcounted. Minting a second id for it would put two
        // registry identities in front of one instance: one set of globals, one
        // descriptor, and a release runnable once per id against shared state.
        QString error;
        runtime_agent::ModuleRegistry::LoadedModule* again = registry.loadSnippet(modulePath, error);
        check(again != nullptr, "loading the same file again succeeds");
        if (again != nullptr) {
            check(again->id == loadedId,
                  "and answers with the module already registered for that object, because "
                  "that is the instance the loader returned");
            check(again == registry.module(loadedId),
                  "as the same record, not a copy of it");
        }
    }

    // A binding made through one adapter, named and released through another.
    //
    // This is the case the whole ownership argument is about, and the version
    // of it I wrote first proved nothing: it compared pristine to pristine,
    // which two unrelated managers would also report, and it compared the
    // registry with itself.
    std::printf("a binding outlives the adapter that made it\n");
    {
        auto* entry = reinterpret_cast<void*>(&cube_step_builtin);
        runtime_agent::PatchManager& patches =
            runtime_agent::PatchRegistry::instance().forEntry(entry);

        auto owner = std::make_unique<ModuleManager>(nullptr);
        QString error;
        runtime_agent::ModuleRegistry::LoadedModule* binder = registry.loadSnippet(modulePath, error);
        check(binder != nullptr, "loaded a module to own the binding");
        if (binder == nullptr) {
            return 1;
        }
        const quint64 binderId = binder->id;

        // Installed and selected directly, because going through the adapter
        // would ask the manifest, and what is being tested is ownership.
        const runtime_agent::LiveTextWriteAdmission admission(
            runtime_agent::WriteAdmissionBasis::AlreadyQuiescent,
            "single-thread",
            "cube_step_builtin",
            "the teardown test is the only thread there is");
        runtime_agent::SingleThreadQuiescer quiet;
        runtime_agent::PatchSite site;
        std::string nativeError;
        if (!runtime_agent::resolvePatchSite(entry, runtime_agent::patchAreaBytes, site,
                                             nativeError)) {
            std::printf("  skip  this build reserved no area: %s\n", nativeError.c_str());
        } else {
            check(patches.installGateway(site, admission, quiet, nativeError),
                  nativeError.empty() ? "installed a gateway" : nativeError.c_str());

            runtime_agent::PatchBinding binding;
            check(patches.bind(reinterpret_cast<void*>(&cube_step_builtin), binderId, binding,
                               nativeError),
                  nativeError.empty() ? "bound a replacement owned by that module"
                                      : nativeError.c_str());

            const QJsonObject held = owner->patchStatus();
            check(held.value(QStringLiteral("mode")).toString() == QStringLiteral("entry"),
                  "the adapter that made it says the entry is replaced");
            check(held.value(QStringLiteral("moduleId")).toString()
                      == QString::number(binderId),
                  "and names the module that owns it");

            owner.reset();

            // The adapter is gone. The binding is not, and neither is what is
            // needed to say whose it is.
            auto successor = std::make_unique<ModuleManager>(nullptr);
            const QJsonObject found = successor->patchStatus();
            check(found.value(QStringLiteral("entryState")).toString()
                      == QStringLiteral("replacement"),
                  "a successor sees the entry still replaced");
            check(found.value(QStringLiteral("moduleId")).toString()
                      == QString::number(binderId),
                  "and names the same module, which is the record that used to die");
            check(successor->module(binderId) != nullptr,
                  "and can still look that module up");

            // And can let it go, which needs the owner id to match.
            check(patches.unbind(binding.id, binderId, nativeError),
                  nativeError.empty() ? "and can release it" : nativeError.c_str());
            check(successor->patchStatus().value(QStringLiteral("mode")).toString()
                      == QStringLiteral("builtin"),
                  "after which nothing is selected");
        }
    }

    std::printf("%s\n", failures == 0 ? "all teardown checks passed" : "teardown checks failed");
    return failures == 0 ? 0 : 1;
}
