#include "agent/module_manager.h"

#include "agent/build_id.h"
#include "agent/patch_area.h"
#include "agent/patch_site.h"
#include "cube_widget.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <dlfcn.h>

namespace {

// Stopping the cube long enough to write its step function's entry.
//
// The animation timer is the only thing that calls it, so pausing the timer is
// this application's whole quiescence story. A general target has to account
// for every thread, which is why the interface is separate from the manager
// that uses it.
class CubeTimerQuiescer final : public runtime_agent::Quiescer {
public:
    explicit CubeTimerQuiescer(CubeWidget* cube)
        : m_cube(cube)
    {
    }

    std::unique_ptr<runtime_agent::QuiescenceLease> acquire(std::string& error) override
    {
        if (m_cube == nullptr) {
            error = "there is no cube widget to stop";
            return nullptr;
        }
        return std::make_unique<TimerLease>(m_cube);
    }

private:
    // Restores only what it stopped, so a timer that was already paused stays
    // paused when the lease goes away.
    class TimerLease final : public runtime_agent::QuiescenceLease {
    public:
        explicit TimerLease(CubeWidget* cube)
            : m_cube(cube)
            , m_wasRunning(cube->isRunning())
        {
            m_cube->setRunning(false);
        }

        ~TimerLease() override
        {
            if (m_wasRunning) {
                m_cube->setRunning(true);
            }
        }

    private:
        CubeWidget* m_cube = nullptr;
        bool m_wasRunning = false;
    };

    CubeWidget* m_cube = nullptr;
};

QString dynamicLoaderError(const QString& prefix)
{
    const char* message = ::dlerror();
    return message != nullptr
        ? QStringLiteral("%1: %2").arg(prefix, QString::fromLocal8Bit(message))
        : prefix;
}

QString pointerString(const void* pointer)
{
    return QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(pointer), 0, 16);
}

void closeFailedModule(void* handle)
{
    if (handle != nullptr) {
        (void)::dlclose(handle);
    }
}

// Whether the module was compiled against the build of the host that is running.
//
// A module compiled with -fno-access-control addresses application members at
// offsets fixed when it was compiled. Those offsets describe the source it saw,
// so a module built from different source describes a different object than the
// one it is handed. The build id is the coarsest possible answer to that
// question and the right one: any change to any translation unit produces a new
// id, and any such change can move a member.
//
// A module that reports nothing is accepted. The host cannot tell one built
// outside this build system from a stale one, and refusing both would rule out
// toolchains rather than answer the question, so the caller reports it instead.
bool targetBuildMatches(void* handle, QString& moduleBuildId, QString& error)
{
    ::dlerror();
    auto reportBuildId = reinterpret_cast<const char* (*)()>(
        ::dlsym(handle, "runtime_agent_target_build_id"));
    if (reportBuildId == nullptr) {
        moduleBuildId.clear();
        return true;
    }

    const char* reported = reportBuildId();
    moduleBuildId = reported != nullptr ? QString::fromLatin1(reported) : QString();

    const QString hostBuildId = runtime_agent::hostBuildId();
    if (hostBuildId.isEmpty() || moduleBuildId.isEmpty()
        || moduleBuildId == hostBuildId) {
        return true;
    }

    error = QStringLiteral(
                "module was compiled against host build %1, and this process is build %2. "
                "A module reads application types at offsets fixed when it was compiled, so "
                "the offsets it holds do not describe this process. Rebuild the module "
                "against this build, or restart the application from the build the module "
                "was compiled against.")
                .arg(moduleBuildId, hostBuildId);
    return false;
}

} // namespace

ModuleManager::ModuleManager(CubeWidget* cube)
    : m_cube(cube)
{
}

ModuleManager::~ModuleManager()
{
    QString ignored;
    (void)resetActivePatch(ignored);
    // Intentionally do not dlclose. Snippets may have installed callbacks whose
    // machine code and static storage live in these modules.
}

ModuleManager::LoadedModule* ModuleManager::loadSnippet(const QString& path, QString& error)
{
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    if (path.isEmpty() || !QFileInfo(absolutePath).isFile()) {
        error = QStringLiteral("snippet module is not a regular file: %1").arg(absolutePath);
        return nullptr;
    }
    const QByteArray encodedPath = QFile::encodeName(absolutePath);
    ::dlerror();
    void* handle = ::dlopen(encodedPath.constData(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        error = dynamicLoaderError(QStringLiteral("dlopen(%1) failed").arg(absolutePath));
        return nullptr;
    }

    // Checked before the descriptor, because a module whose offsets do not
    // describe this process should be refused on that ground and told so.
    QString moduleBuildId;
    if (!targetBuildMatches(handle, moduleBuildId, error)) {
        closeFailedModule(handle);
        return nullptr;
    }

    ::dlerror();
    auto init = reinterpret_cast<RuntimeAgentSnippetInit>(
        ::dlsym(handle, "runtime_agent_snippet_init"));
    if (init == nullptr) {
        error = dynamicLoaderError(QStringLiteral("snippet init symbol not found"));
        closeFailedModule(handle);
        return nullptr;
    }

    const RuntimeAgentSnippet* descriptor = init();
    if (descriptor == nullptr
        || descriptor->abi_version != RUNTIME_AGENT_ABI
        || descriptor->struct_size < sizeof(RuntimeAgentSnippet)
        || descriptor->run == nullptr) {
        error = QStringLiteral("invalid RuntimeAgentSnippet descriptor");
        closeFailedModule(handle);
        return nullptr;
    }

    auto module = std::make_unique<LoadedModule>();
    module->id = m_nextId++;
    module->path = absolutePath;
    module->name = QString::fromUtf8(descriptor->name_utf8 != nullptr
                                    ? descriptor->name_utf8 : "unnamed snippet");
    module->kind = Kind::Snippet;
    module->handle = handle;
    module->snippet = descriptor;
    module->targetBuildId = moduleBuildId;
    module->stamped = !moduleBuildId.isEmpty();
    if (!module->stamped) {
        qWarning().noquote()
            << "runtime-agent: loaded unstamped module" << absolutePath
            << "which reports no host build id, so its offsets into application types are"
            << "unchecked against this process";
    }
    return insertModule(std::move(module));
}

ModuleManager::LoadedModule* ModuleManager::loadCubePatch(const QString& path, QString& error)
{
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    if (path.isEmpty() || !QFileInfo(absolutePath).isFile()) {
        error = QStringLiteral("cube patch module is not a regular file: %1").arg(absolutePath);
        return nullptr;
    }
    const QByteArray encodedPath = QFile::encodeName(absolutePath);
    ::dlerror();
    void* handle = ::dlopen(encodedPath.constData(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        error = dynamicLoaderError(QStringLiteral("dlopen(%1) failed").arg(absolutePath));
        return nullptr;
    }

    QString moduleBuildId;
    if (!targetBuildMatches(handle, moduleBuildId, error)) {
        closeFailedModule(handle);
        return nullptr;
    }

    ::dlerror();
    auto init = reinterpret_cast<CubeStepPatchInit>(
        ::dlsym(handle, "cube_step_patch_init"));
    if (init == nullptr) {
        error = dynamicLoaderError(QStringLiteral("cube patch init symbol not found"));
        closeFailedModule(handle);
        return nullptr;
    }

    const CubeStepPatch* descriptor = init();
    if (descriptor == nullptr
        || descriptor->abi_version != CUBE_STEP_ABI
        || descriptor->struct_size < sizeof(CubeStepPatch)
        || descriptor->step == nullptr) {
        error = QStringLiteral("invalid CubeStepPatch descriptor");
        closeFailedModule(handle);
        return nullptr;
    }

    auto module = std::make_unique<LoadedModule>();
    module->id = m_nextId++;
    module->path = absolutePath;
    module->name = QString::fromUtf8(descriptor->name_utf8 != nullptr
                                    ? descriptor->name_utf8 : "unnamed cube patch");
    module->kind = Kind::CubePatch;
    module->handle = handle;
    module->cubePatch = descriptor;
    module->targetBuildId = moduleBuildId;
    module->stamped = !moduleBuildId.isEmpty();
    return insertModule(std::move(module));
}

ModuleManager::LoadedModule* ModuleManager::module(const quint64 id) const
{
    const auto iterator = m_modules.find(id);
    return iterator == m_modules.end() ? nullptr : iterator->second.get();
}

QJsonArray ModuleManager::list() const
{
    QJsonArray result;
    for (const auto& [id, module] : m_modules) {
        Q_UNUSED(id);
        result.append(moduleJson(*module));
    }
    return result;
}

bool ModuleManager::activateDispatchPatch(const quint64 id, QString& error)
{
    LoadedModule* loaded = module(id);
    if (loaded == nullptr || loaded->kind != Kind::CubePatch || loaded->cubePatch == nullptr) {
        error = QStringLiteral("module %1 is not a cube patch").arg(id);
        return false;
    }
    if (!resetActivePatch(error)) {
        return false;
    }

    m_activeDispatchModule = id;
    m_cube->installDispatchStep(
        loaded->cubePatch->step,
        QStringLiteral("dispatch: %1").arg(loaded->name));
    return true;
}

bool ModuleManager::activateEntryPatch(const quint64 id, QString& error)
{
    LoadedModule* loaded = module(id);
    if (loaded == nullptr || loaded->kind != Kind::CubePatch || loaded->cubePatch == nullptr) {
        error = QStringLiteral("module %1 is not a cube patch").arg(id);
        return false;
    }
    if (!resetActivePatch(error)) {
        return false;
    }

#if !defined(__linux__) || !defined(__x86_64__)
    error = QStringLiteral("entry patch mode is implemented only for Linux/x86-64");
    return false;
#else
    if (QThread::currentThread() != m_cube->thread()) {
        error = QStringLiteral("entry patches must be installed from the cube's owning thread");
        return false;
    }

    runtime_agent::PatchSite site;
    std::string nativeError;
    if (!runtime_agent::resolvePatchSite(reinterpret_cast<void*>(&cube_step_builtin),
                                         runtime_agent::patchAreaBytes,
                                         site,
                                         nativeError)) {
        error = QString::fromStdString(nativeError);
        return false;
    }
    site.name = QStringLiteral("cube_step_builtin").toStdString();

    CubeTimerQuiescer quiescer(m_cube);
    if (!m_patches.activate(site,
                            reinterpret_cast<void*>(loaded->cubePatch->step),
                            quiescer,
                            nativeError)) {
        error = QString::fromStdString(nativeError);
        if (m_patches.status().state == runtime_agent::PatchState::RecoveryRequired) {
            // The manager is holding execution stopped. Recording the module
            // keeps rollback able to name what is installed.
            m_activeEntryModule = id;
            m_cube->setActivePatchLabel(
                QStringLiteral("entry (recovery required): %1").arg(loaded->name));
        }
        return false;
    }

    m_activeEntryModule = id;
    m_cube->setActivePatchLabel(QStringLiteral("entry: %1").arg(loaded->name));
    return true;
#endif
}

bool ModuleManager::rollback(QString& error)
{
    return resetActivePatch(error);
}

QJsonObject ModuleManager::patchStatus() const
{
    const runtime_agent::PatchStatus patch = m_patches.status();
    QString mode = QStringLiteral("builtin");
    quint64 moduleId = 0;
    if (m_activeEntryModule != 0 && m_patches.replacementSelected()) {
        mode = QStringLiteral("entry");
        moduleId = m_activeEntryModule;
    } else if (m_activeDispatchModule != 0) {
        mode = QStringLiteral("dispatch");
        moduleId = m_activeDispatchModule;
    }

    QJsonObject result{
        {QStringLiteral("mode"), mode},
        {QStringLiteral("moduleId"), moduleId == 0 ? QJsonValue() : QJsonValue(QString::number(moduleId))},
        {QStringLiteral("target"), pointerString(reinterpret_cast<void*>(&cube_step_builtin))},
        {QStringLiteral("patchAddress"), patch.site.has_value()
            ? QJsonValue(pointerString(patch.site->patchAddress))
            : QJsonValue()},
        {QStringLiteral("reservedEntryBytes"),
         static_cast<qint64>(runtime_agent::patchAreaBytes)},
        // What the entry itself holds, which is a separate question from what
        // is driving the cube. A gateway naming its continuation runs the
        // original function, so the entry can be rewritten while mode is
        // builtin. One field rather than several booleans, because most
        // combinations of those would describe nothing that can happen.
        {QStringLiteral("entryState"),
         QString::fromLatin1(runtime_agent::describe(patch.state))},
        {QStringLiteral("gatewaySlot"), patch.slotAddress != nullptr
            ? QJsonValue(pointerString(patch.slotAddress))
            : QJsonValue()},
    };
    if (LoadedModule* loaded = module(moduleId); loaded != nullptr) {
        result.insert(QStringLiteral("module"), moduleJson(*loaded));
    }
    return result;
}

ModuleManager::LoadedModule* ModuleManager::insertModule(std::unique_ptr<LoadedModule> module)
{
    const quint64 id = module->id;
    LoadedModule* pointer = module.get();
    m_modules.emplace(id, std::move(module));
    return pointer;
}

bool ModuleManager::resetActivePatch(QString& error)
{
    if (m_patches.replacementSelected()) {
        CubeTimerQuiescer quiescer(m_cube);
        std::string nativeError;
        if (!m_patches.rollback(quiescer, nativeError)) {
            error = QString::fromStdString(nativeError);
            return false;
        }
    }

    m_activeEntryModule = 0;
    m_activeDispatchModule = 0;
    // Dispatch is the application's own seam, so putting it back is the
    // adapter's job and not the patch manager's.
    m_cube->resetDispatchStep();
    return true;
}


QJsonObject ModuleManager::moduleJson(const LoadedModule& module)
{
    QJsonObject json{
        {QStringLiteral("id"), QString::number(module.id)},
        {QStringLiteral("name"), module.name},
        {QStringLiteral("path"), module.path},
        {QStringLiteral("kind"), module.kind == Kind::Snippet
            ? QStringLiteral("snippet") : QStringLiteral("cubePatch")},
        {QStringLiteral("handle"), pointerString(module.handle)},
        // A stamped module agrees with the running executable, because one that
        // disagreed was refused. An unstamped one was never checked.
        {QStringLiteral("stamped"), module.stamped},
    };
    if (module.stamped) {
        json.insert(QStringLiteral("targetBuildId"), module.targetBuildId);
    }
    if (module.kind != Kind::Snippet) {
        return json;
    }

    // How the module was last driven, so a caller can see what its release
    // would run under without having to remember what it asked for earlier.
    json.insert(QStringLiteral("declaresRelease"), module.declaresRelease());
    json.insert(QStringLiteral("hadSuccessfulRun"), module.hadSuccessfulRun);
    json.insert(QStringLiteral("hadAttemptedRun"), module.hadAttemptedRun);
    if (!module.hadSuccessfulRun && module.hadAttemptedRun) {
        // The run that installed something and then failed is the interesting
        // case here, so say where it ran even though it did not complete.
        json.insert(QStringLiteral("lastAttemptedExecutor"), module.lastAttemptedExecutor);
        if (module.lastAttemptedExecutor == QStringLiteral("object")) {
            // Same diagnosis the successful record offers: a release resolving
            // through this record fails when the object is gone, and a caller
            // can see that coming rather than being told after the fact.
            json.insert(QStringLiteral("lastAttemptedTargetAlive"),
                        !module.lastAttemptedTarget.isNull());
        }
    }
    if (module.hadSuccessfulRun) {
        json.insert(QStringLiteral("lastExecutor"), module.lastExecutor);
        if (module.lastExecutor == QStringLiteral("object")) {
            json.insert(QStringLiteral("lastTargetAlive"), !module.lastTarget.isNull());
            json.insert(QStringLiteral("lastTarget"), module.lastTarget.isNull()
                ? QString()
                : module.lastTarget->objectName());
        }
    }
    return json;
}
