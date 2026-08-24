#include "agent/module_manager.h"

#include "cube_widget.h"

#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <dlfcn.h>

namespace {

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

    ::dlerror();
    auto init = reinterpret_cast<RuntimeAgentSnippetInitV1>(
        ::dlsym(handle, "runtime_agent_snippet_init_v1"));
    if (init == nullptr) {
        error = dynamicLoaderError(QStringLiteral("snippet init symbol not found"));
        closeFailedModule(handle);
        return nullptr;
    }

    const RuntimeAgentSnippetV1* descriptor = init();
    if (descriptor == nullptr
        || descriptor->abi_version != RUNTIME_AGENT_ABI_V1
        || descriptor->struct_size < sizeof(RuntimeAgentSnippetV1)
        || descriptor->run == nullptr) {
        error = QStringLiteral("invalid RuntimeAgentSnippetV1 descriptor");
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

    ::dlerror();
    auto init = reinterpret_cast<CubeStepPatchInitV1>(
        ::dlsym(handle, "cube_step_patch_init_v1"));
    if (init == nullptr) {
        error = dynamicLoaderError(QStringLiteral("cube patch init symbol not found"));
        closeFailedModule(handle);
        return nullptr;
    }

    const CubeStepPatchV1* descriptor = init();
    if (descriptor == nullptr
        || descriptor->abi_version != CUBE_STEP_ABI_V1
        || descriptor->struct_size < sizeof(CubeStepPatchV1)
        || descriptor->step == nullptr) {
        error = QStringLiteral("invalid CubeStepPatchV1 descriptor");
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

    const bool wasRunning = m_cube->isRunning();
    m_cube->setRunning(false);
    std::string nativeError;
    const bool applied = m_entryHotpatch.apply(
        reinterpret_cast<void*>(&cube_step_builtin_v1),
        reinterpret_cast<void*>(loaded->cubePatch->step),
        16,
        nativeError);
    if (wasRunning) {
        m_cube->setRunning(true);
    }
    if (!applied) {
        error = QString::fromStdString(nativeError);
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
    QString mode = QStringLiteral("builtin");
    quint64 moduleId = 0;
    if (m_activeEntryModule != 0) {
        mode = QStringLiteral("entry");
        moduleId = m_activeEntryModule;
    } else if (m_activeDispatchModule != 0) {
        mode = QStringLiteral("dispatch");
        moduleId = m_activeDispatchModule;
    }

    QJsonObject result{
        {QStringLiteral("mode"), mode},
        {QStringLiteral("moduleId"), moduleId == 0 ? QJsonValue() : QJsonValue(QString::number(moduleId))},
        {QStringLiteral("target"), pointerString(reinterpret_cast<void*>(&cube_step_builtin_v1))},
        {QStringLiteral("patchAddress"), m_entryHotpatch.active()
            ? QJsonValue(pointerString(m_entryHotpatch.patchAddress()))
            : QJsonValue()},
        {QStringLiteral("reservedEntryBytes"), 16},
        {QStringLiteral("entryPatchActive"), m_entryHotpatch.active()},
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
    const bool wasRunning = m_cube->isRunning();
    m_cube->setRunning(false);

    if (m_entryHotpatch.active()) {
        std::string nativeError;
        if (!m_entryHotpatch.rollback(nativeError)) {
            if (wasRunning) {
                m_cube->setRunning(true);
            }
            error = QString::fromStdString(nativeError);
            return false;
        }
    }

    m_activeEntryModule = 0;
    m_activeDispatchModule = 0;
    m_cube->resetDispatchStep();
    if (wasRunning) {
        m_cube->setRunning(true);
    }
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
    };
    if (module.kind != Kind::Snippet) {
        return json;
    }

    // How the module was last driven, so a caller can see what its release
    // would run under without having to remember what it asked for earlier.
    json.insert(QStringLiteral("declaresRelease"), module.declaresRelease());
    json.insert(QStringLiteral("hadSuccessfulRun"), module.hadSuccessfulRun);
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
