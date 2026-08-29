#include "agent/module_registry.h"

#include "agent/build_id.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>

#include <atomic>

#include <dlfcn.h>

namespace runtime_agent {
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
// toolchains without answering the question, so the caller reports it instead.
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

    const QString host = hostBuildId();
    if (host.isEmpty() || moduleBuildId.isEmpty() || moduleBuildId == host) {
        return true;
    }

    error = QStringLiteral(
                "module was compiled against host build %1, and this process is build %2. "
                "A module reads application types at offsets fixed when it was compiled, so "
                "the offsets it holds do not describe this process. Rebuild the module "
                "against this build, or restart the application from the build the module "
                "was compiled against.")
                .arg(moduleBuildId, host);
    return false;
}

} // namespace

quint64 ModuleRegistry::nextId()
{
    static std::atomic<quint64> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

void ModuleRegistry::closeUnadopted(void* const handle)
{
    if (handle != nullptr) {
        (void)::dlclose(handle);
    }
}

void* ModuleRegistry::open(const QString& path, QString& buildId, QString& error)
{
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    if (path.isEmpty() || !QFileInfo(absolutePath).isFile()) {
        error = QStringLiteral("module is not a regular file: %1").arg(absolutePath);
        return nullptr;
    }

    const QByteArray encodedPath = QFile::encodeName(absolutePath);
    ::dlerror();
    void* handle = ::dlopen(encodedPath.constData(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        error = dynamicLoaderError(QStringLiteral("dlopen(%1) failed").arg(absolutePath));
        return nullptr;
    }

    // Checked before any descriptor, because a module whose offsets do not
    // describe this process should be refused on that ground and told so.
    if (!targetBuildMatches(handle, buildId, error)) {
        closeUnadopted(handle);
        return nullptr;
    }

    // Said here, so every kind of module gets the same warning. Only the
    // snippet loader used to say it, so a cube patch built without the define
    // loaded in silence while a snippet said so, and the two cube patch
    // targets are in fact not stamped.
    if (buildId.isEmpty()) {
        qWarning().noquote()
            << "runtime-agent: loaded unstamped module" << absolutePath
            << "which reports no host build id, so its offsets into application types are"
            << "unchecked against this process";
    }
    return handle;
}

ModuleRegistry::LoadedModule* ModuleRegistry::adopt(std::unique_ptr<LoadedModule> module)
{
    const quint64 id = module->id;
    m_modules.emplace(id, std::move(module));
    return m_modules.at(id).get();
}

ModuleRegistry::LoadedModule* ModuleRegistry::loadSnippet(const QString& path, QString& error)
{
    QString buildId;
    void* handle = open(path, buildId, error);
    if (handle == nullptr) {
        return nullptr;
    }

    ::dlerror();
    auto init = reinterpret_cast<RuntimeAgentSnippetInit>(
        ::dlsym(handle, "runtime_agent_snippet_init"));
    if (init == nullptr) {
        error = dynamicLoaderError(QStringLiteral("snippet init symbol not found"));
        closeUnadopted(handle);
        return nullptr;
    }

    const RuntimeAgentSnippet* descriptor = init();
    if (descriptor == nullptr
        || descriptor->abi_version != RUNTIME_AGENT_ABI
        || descriptor->struct_size < sizeof(RuntimeAgentSnippet)
        || descriptor->run == nullptr) {
        error = QStringLiteral("invalid RuntimeAgentSnippet descriptor");
        closeUnadopted(handle);
        return nullptr;
    }

    auto module = std::make_unique<LoadedModule>();
    module->id = nextId();
    module->path = QFileInfo(path).absoluteFilePath();
    module->name = QString::fromUtf8(descriptor->name_utf8 != nullptr
                                         ? descriptor->name_utf8
                                         : "unnamed snippet");
    module->kind = QStringLiteral("snippet");
    module->handle = handle;
    module->snippet = descriptor;
    module->descriptor = descriptor;
    module->targetBuildId = buildId;
    module->stamped = !buildId.isEmpty();
    return adopt(std::move(module));
}

ModuleRegistry::LoadedModule* ModuleRegistry::module(const quint64 id) const
{
    const auto it = m_modules.find(id);
    return it != m_modules.end() ? it->second.get() : nullptr;
}

QJsonObject ModuleRegistry::describe(const LoadedModule& module)
{
    QJsonObject json{
        {QStringLiteral("id"), QString::number(module.id)},
        {QStringLiteral("name"), module.name},
        {QStringLiteral("path"), module.path},
        {QStringLiteral("kind"), module.kind},
        {QStringLiteral("handle"), pointerString(module.handle)},
        // A stamped module agrees with the running executable, because one that
        // reported a different build was refused at load. An unstamped one was
        // never checked.
        {QStringLiteral("stamped"), module.stamped},
        {QStringLiteral("declaresRelease"), module.declaresRelease()},
        {QStringLiteral("hadSuccessfulRun"), module.hadSuccessfulRun},
        {QStringLiteral("hadAttemptedRun"), module.hadAttemptedRun},
    };
    if (module.stamped) {
        json.insert(QStringLiteral("targetBuildId"), module.targetBuildId);
    }
    if (module.hadSuccessfulRun) {
        json.insert(QStringLiteral("lastExecutor"), module.lastExecutor);
        json.insert(QStringLiteral("lastTargetAlive"), !module.lastTarget.isNull());
    }
    if (module.hadAttemptedRun) {
        json.insert(QStringLiteral("lastAttemptedExecutor"), module.lastAttemptedExecutor);
        // Same diagnosis the successful record offers: a release resolving
        // through this record fails when the object is gone, and a caller can
        // see that coming instead of being told after the fact.
        json.insert(QStringLiteral("lastAttemptedTargetAlive"),
                    !module.lastAttemptedTarget.isNull());
    }
    return json;
}

QJsonArray ModuleRegistry::list() const
{
    QJsonArray array;
    for (const auto& [id, module] : m_modules) {
        array.append(describe(*module));
    }
    return array;
}

} // namespace runtime_agent
