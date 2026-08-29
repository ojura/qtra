#include "agent/module_manager.h"

#include "agent/build_id.h"
#include "agent/patch_area.h"
#include "agent/coverage_manifest.h"
#include "agent/quiescence_providers.h"
#include "agent/patch_site.h"
#include "cube_widget.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <atomic>

#include <dlfcn.h>

namespace {

// Why writing this target's entry is safe here.
//
// The only thing that calls it is the widget's animation slot, and that runs on
// the thread that owns the widget. Every path that writes the entry, the
// protocol handler and the host binding calls, is refused unless it is already
// on that thread. So a write happens between two ticks of a single-threaded
// event loop, with the target not on the stack, and nothing else can be inside
// it.
//
// That is the reason, and it is worth naming precisely, because stopping the
// animation timer also looks like a reason and is not one: the timer cannot
// fire while this thread is in the handler doing the writing. Pausing it would
// change what the cube does without changing what makes the write safe, and
// reporting it as the reason would describe the wrong thing.
class SameThreadRequestBoundary final : public runtime_agent::Quiescer {
public:
    explicit SameThreadRequestBoundary(CubeWidget* cube)
        : m_cube(cube)
    {
    }

    [[nodiscard]] const char* name() const noexcept override
    {
        return "same-thread-request-boundary";
    }

    std::unique_ptr<runtime_agent::QuiescenceLease> acquire(std::string& error) override
    {
        if (m_cube == nullptr) {
            error = "there is no widget whose thread this could be";
            return nullptr;
        }
        // Checked again here and not assumed from the caller's check, since
        // this is the object claiming the write is safe.
        if (QThread::currentThread() != m_cube->thread()) {
            error = "this is not the thread that owns the target, so occupying it proves "
                    "nothing about what else may be running the target";
            return nullptr;
        }
        // Nothing is stopped, so nothing is restored. Holding this thread is
        // what the claim rests on, and the caller already holds it.
        return std::make_unique<runtime_agent::QuiescenceLease>();
    }

private:
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

namespace {

// Module ids, never reused for the life of the process.
//
// A binding names the module that owns it, and bindings now outlive the manager
// that made them: the registry keeps the entry's generations whatever happens to
// whoever installed them. A second manager numbering from one again would hand a
// new module the identity of one whose binding is still selected, and the rule
// that nobody can release somebody else's work would then be checking two
// different modules against one number.
std::uint64_t nextModuleId()
{
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

namespace {

// What the write was admitted under, in the terms the record keeps.
//
// Built here because this is where the evidence was read and judged. The patch
// layer is handed the conclusion and never looks inside it.
runtime_agent::LiveTextWriteAdmission admissionFor(
    const runtime_agent::Quiescer& quiescer,
    const runtime_agent::CoverageDecision& decision)
{
    return runtime_agent::LiveTextWriteAdmission(
        runtime_agent::WriteAdmissionBasis::RequestBoundary,
        quiescer.name(),
        decision.target.toStdString(),
        QStringLiteral("caller execution domain %1")
            .arg(decision.domainStrength.isEmpty() ? QStringLiteral("absent")
                                                   : decision.domainStrength)
            .toStdString(),
        decision.manifestBuildId.toStdString());
}

} // namespace

ModuleManager::ModuleManager(CubeWidget* cube)
    : m_cube(cube)
    , m_patches(runtime_agent::PatchRegistry::instance().forEntry(
          reinterpret_cast<void*>(&cube_step_builtin)))
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
    module->id = nextModuleId();
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
    module->id = nextModuleId();
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

namespace {

// Where the build left its decision, beside the artifacts it describes.
//
// The path is compiled in, and the build directory being present is something
// this assumes. An executable away from its build tree finds no manifest and
// refuses every activation, which is the direction to fail in.
//
// Taking the path from whoever asks would defeat the point. The build id is
// readable, so anyone who could name a path could write a manifest carrying
// that id with coverage complete and a declared domain, and the decision would
// become an input. What gives a manifest its meaning is that the build wrote it
// and that it names this binary and this function, which is what the build id
// and target checks establish.
QString manifestPath()
{
    return QStringLiteral("%1/coverage-manifest.json").arg(QStringLiteral(DEMO_BUILD_DIR));
}

} // namespace

bool ModuleManager::installIfNeeded(const runtime_agent::PatchSite& site,
                                    const runtime_agent::CoverageDecision& decision,
                                    runtime_agent::Quiescer& quiescer,
                                    std::string& error)
{
    if (m_patches.state() != runtime_agent::PatchState::NoGateway) {
        return true;
    }
    // The decision that allowed this write, handed over by the caller that got
    // it. Passing it instead of keeping it is what stops an admission being
    // built out of nothing when somebody adds a path that forgot to ask.
    return m_patches.installGateway(site, admissionFor(quiescer, decision), quiescer, error);
}

runtime_agent::CoverageDecision ModuleManager::readDecision() const
{
    return runtime_agent::readCoverageManifest(
        manifestPath(), runtime_agent::hostBuildId(), QStringLiteral("cube_step_builtin"));
}

QJsonObject ModuleManager::coverage() const
{
    const runtime_agent::CoverageDecision decision = readDecision();
    return QJsonObject{
        {QStringLiteral("coverage"), decision.present() ? QJsonValue(decision.coverage)
                                                        : QJsonValue()},
        {QStringLiteral("allow"), decision.allow},
        {QStringLiteral("reason"), decision.reason},
        {QStringLiteral("manifestBuildId"), decision.manifestBuildId.isEmpty()
            ? QJsonValue() : QJsonValue(decision.manifestBuildId)},
    };
}

bool ModuleManager::admits(const bool acceptIncompleteCoverage,
                           runtime_agent::CoverageDecision& decision,
                           QString& error)
{
    // What the build established about replacing this function. Refused by
    // default: a build that recorded nothing has not been asked whether the
    // replacement reaches every call, and treating silence as approval is the
    // thing the manifest exists to prevent.
    decision = readDecision();

    // Asked of every selection, because a replacement that misses callers
    // misses them however it came to be chosen.
    if (!runtime_agent::admitsReplacementEffect(decision, acceptIncompleteCoverage, error)) {
        return false;
    }

    // Installing the gateway rewrites the target's entry. Once one is there,
    // choosing what runs is a single aligned store into its slot: no byte of
    // the target changes, so the claim about which threads reach it is a claim
    // about an operation that is not happening.
    if (m_patches.state() == runtime_agent::PatchState::NoGateway) {
        if (!runtime_agent::authorizesLiveTextWrite(decision, error)) {
            return false;
        }
    }
    return true;
}

bool ModuleManager::activateEntryPatch(const quint64 id,
                                       const bool acceptIncompleteCoverage,
                                       QString& error)
{
    LoadedModule* loaded = module(id);
    if (loaded == nullptr || loaded->kind != Kind::CubePatch || loaded->cubePatch == nullptr) {
        error = QStringLiteral("module %1 is not a cube patch").arg(id);
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

    runtime_agent::CoverageDecision decision;
    if (!admits(acceptIncompleteCoverage, decision, error)) {
        return false;
    }

    // A site is a measurement of untouched NOPs, so it can only be taken before
    // anything is installed. Once a gateway exists the manager's record is the
    // answer, and resolving again would find the gateway's own bytes and refuse.
    runtime_agent::PatchSite site;
    std::string nativeError;
    if (const auto recorded = m_patches.status().site; recorded.has_value()) {
        site = *recorded;
    } else {
        if (!runtime_agent::resolvePatchSite(reinterpret_cast<void*>(&cube_step_builtin),
                                             runtime_agent::patchAreaBytes,
                                             site,
                                             nativeError)) {
            error = QString::fromStdString(nativeError);
            return false;
        }
        site.name = QStringLiteral("cube_step_builtin").toStdString();
    }

    const std::uint64_t previousBinding = m_entryBinding;
    const quint64 previousModule = m_activeEntryModule;

    // Before anything is written. Installing a gateway is permanent, and a
    // request that will be refused for its replacement must not leave one
    // behind: the entry would run the original through the slot, so nothing
    // breaks, but a refused request would have rewritten the entry. bind checks
    // again against the site the gateway was actually installed at, which is
    // the one the jump is taken from.
    if (!runtime_agent::replacementIsReachable(
            site, reinterpret_cast<void*>(loaded->cubePatch->step), nativeError)) {
        error = QString::fromStdString(nativeError);
        return false;
    }

    SameThreadRequestBoundary quiescer(m_cube);
    runtime_agent::PatchBinding binding;
    if (!installIfNeeded(site, decision, quiescer, nativeError)) {
        error = QString::fromStdString(nativeError);
        if (m_patches.state() == runtime_agent::PatchState::RecoveryRequired) {
            m_activeEntryModule = id;
            m_cube->setActivePatchLabel(
                QStringLiteral("entry (recovery required): %1").arg(loaded->name));
            m_cube->update();
        }
        return false;
    }
    if (!m_patches.bind(reinterpret_cast<void*>(loaded->cubePatch->step),
                        id,
                        binding,
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

    // The old binding goes only once the new one is selected. Releasing first
    // meant a refusal further down left the previous replacement deselected and
    // the label describing something that was no longer running. The generations
    // exist so a switch does not have to pass through nothing.
    if (previousBinding != 0) {
        std::string releaseError;
        if (!m_patches.unbind(previousBinding, previousModule, releaseError)) {
            // Put it back the way it was, so a failed switch changes nothing.
            std::string undoError;
            (void)m_patches.unbind(binding.id, id, undoError);
            error = QString::fromStdString(releaseError);
            return false;
        }
    }

    m_entryBinding = binding.id;
    m_activeEntryModule = id;
    m_cube->setActivePatchLabel(QStringLiteral("entry: %1").arg(loaded->name));
    return true;
#endif
}

int ModuleManager::bindReplacement(void* target,
                                   void* replacement,
                                   const quint64 owner,
                                   const bool acceptIncompleteCoverage,
                                   runtime_agent::PatchBinding& binding,
                                   QString& error)
{
    // A module keeps a copy of the host struct and may call this from anywhere,
    // including a worker. Everything below reaches the widget: the quiescence
    // provider stops its timer and the result relabels it, and Qt forbids both
    // from another thread. Refusing is the answer, because doing the work on the
    // wrong thread fails as a race inside the process instead of as an error
    // anyone can see.
    if (QThread::currentThread() != m_cube->thread()) {
        error = QStringLiteral(
            "patch_bind reaches the target's own widget, so it has to be called from "
            "the thread that owns it. A snippet should bind from the executor its "
            "target runs on");
        return -3;
    }

    if (target != reinterpret_cast<void*>(&cube_step_builtin)) {
        error = QStringLiteral(
            "this runtime resolves one target so far, cube_step_builtin at %1. The host "
            "interface is not limited to it; what is missing is a way to find another "
            "function's prepared area and to stop whatever might be running it")
            .arg(QStringLiteral("0x%1").arg(
                reinterpret_cast<quintptr>(&cube_step_builtin), 0, 16));
        return -1;
    }

    // The same admission the protocol answers to. Without this a module reaches
    // a write that a request over the socket would have refused, and the build's
    // decision only binds whoever happened to ask the other way.
    runtime_agent::CoverageDecision decision;
    if (!admits(acceptIncompleteCoverage, decision, error)) {
        return -1;
    }

    runtime_agent::PatchSite site;
    std::string nativeError;
    if (const auto recorded = m_patches.status().site; recorded.has_value()) {
        site = *recorded;
    } else if (!runtime_agent::resolvePatchSite(target,
                                                runtime_agent::patchAreaBytes,
                                                site,
                                                nativeError)) {
        error = QString::fromStdString(nativeError);
        return -1;
    } else {
        site.name = QStringLiteral("cube_step_builtin").toStdString();
    }

    // Before anything is written. Installing a gateway is permanent, and a
    // request that will be refused for its replacement must not leave one
    // behind: the entry would run the original through the slot, so nothing
    // breaks, but a refused request would have rewritten the entry. bind checks
    // again against the site the gateway was actually installed at, which is
    // the one the jump is taken from.
    if (!runtime_agent::replacementIsReachable(site, replacement, nativeError)) {
        error = QString::fromStdString(nativeError);
        return -1;
    }

    SameThreadRequestBoundary quiescer(m_cube);
    if (!installIfNeeded(site, decision, quiescer, nativeError)) {
        error = QString::fromStdString(nativeError);
        return m_patches.state() == runtime_agent::PatchState::RecoveryRequired ? -3 : -2;
    }
    if (!m_patches.bind(replacement, owner, binding, nativeError)) {
        error = QString::fromStdString(nativeError);
        return m_patches.state() == runtime_agent::PatchState::RecoveryRequired ? -3 : -2;
    }
    m_cube->setActivePatchLabel(QStringLiteral("entry: binding %1").arg(binding.id));
    return 0;
}

int ModuleManager::releaseBinding(const std::uint64_t bindingId,
                                  const quint64 owner,
                                  QString& error)
{
    // Same reason as binding: this relabels the widget when the last binding
    // goes, and that is not a cross-thread operation.
    if (QThread::currentThread() != m_cube->thread()) {
        error = QStringLiteral(
            "patch_unbind reaches the target's own widget, so it has to be called from "
            "the thread that owns it");
        return -3;
    }

    std::string nativeError;
    if (!m_patches.unbind(bindingId, owner, nativeError)) {
        error = QString::fromStdString(nativeError);
        return error.contains(QStringLiteral("another module")) ? -2 : -1;
    }
    if (!m_patches.replacementSelected()) {
        m_cube->setActivePatchLabel(QStringLiteral("builtin"));
    }
    return 0;
}

bool ModuleManager::rollback(QString& error)
{
    return resetActivePatch(error);
}

QJsonObject ModuleManager::patchStatus() const
{
    const runtime_agent::PatchStatus patch = m_patches.status();

    // Both from the manager, which is the one thing that knows what the entry
    // reaches. Reporting used to read this side's own record of the protocol's
    // binding, which no host patch_bind ever wrote, so a module binding through
    // the ABI left status saying builtin while the entry held its replacement.
    //
    // The owner recorded with a generation is the module that bound it, on
    // either path, so it answers this without a second copy to keep in step.
    const bool replacing = m_patches.replacementSelected();
    const QString mode = replacing ? QStringLiteral("entry") : QStringLiteral("builtin");
    const quint64 moduleId = replacing ? patch.selectedOwner : 0;

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
        // builtin. One field, not several booleans, because most
        // combinations of those would describe nothing that can happen.
        {QStringLiteral("coverage"), coverage()},
        {QStringLiteral("entryState"),
         QString::fromLatin1(runtime_agent::describe(patch.state))},
        {QStringLiteral("gatewaySlot"), patch.slotAddress != nullptr
            ? QJsonValue(pointerString(patch.slotAddress))
            : QJsonValue()},
        // What made the one code write safe, and what it was judged against. A
        // thread count above one is not proof the write was safe; it is the
        // number the policy had to answer for.
        {QStringLiteral("quiescedBy"), patch.quiescedBy.empty()
            ? QJsonValue()
            : QJsonValue(QString::fromStdString(patch.quiescedBy))},
        {QStringLiteral("threadsAtInstall"), patch.threadsAtInstall.has_value()
            ? QJsonValue(static_cast<qint64>(*patch.threadsAtInstall))
            : QJsonValue()},
    };
    // What the write that left this entry needing recovery was admitted under.
    // Present only then, because that is the only write anyone still has to
    // finish, and a caller looking at a stuck process can say what it was made
    // under without asking a file that may have changed since.
    if (patch.recoveryAdmission.has_value()) {
        const runtime_agent::LiveTextWriteAdmission& admitted = *patch.recoveryAdmission;
        result.insert(QStringLiteral("recoveryAdmission"),
                      QJsonObject{
                          {QStringLiteral("basis"),
                           QString::fromLatin1(runtime_agent::describe(admitted.basis()))},
                          {QStringLiteral("provider"),
                           QString::fromStdString(admitted.provider())},
                          {QStringLiteral("target"), QString::fromStdString(admitted.target())},
                          {QStringLiteral("buildId"), admitted.buildId().empty()
                              ? QJsonValue()
                              : QJsonValue(QString::fromStdString(admitted.buildId()))},
                          {QStringLiteral("detail"), QString::fromStdString(admitted.detail())},
                      });
    }
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
    // The one way back from an install that changed bytes and could not finish.
    // Without this the state told a caller to recover and no command could.
    if (m_patches.state() == runtime_agent::PatchState::RecoveryRequired) {
        // Recovery restores the entry's own bytes, which is a code write, so
        // reaching it from the wrong thread is the same hazard installing from
        // one would be.
        if (QThread::currentThread() != m_cube->thread()) {
            error = QStringLiteral("recovery writes the target's entry, so it has to run "
                                   "on the thread that owns it");
            return false;
        }
        // What admitted the install admits putting it back. Reading the
        // manifest again would ask about the file as it stands now: a rebuild
        // between the failed install and this call gives the binary a new build
        // id, the manifest on disk describes that one, and recovery would be
        // refused with the entry left mid-install and nothing able to finish
        // it. The bytes being restored are this process's own, so the decision
        // taken when they were written is the one that governs restoring them.
        // Nothing is asked of the manifest here. What admitted the write is
        // kept with the write, in the record the failed install left, and a
        // record cannot exist without one. Asking again would be asking a
        // question about now that the answer describes about then, and on a
        // file that may have been replaced since.
        //
        // What makes recovery safe is the lease that install is still holding
        // and the check above, which is a fact about this instant.
        std::string nativeError;
        if (!m_patches.recover(nativeError)) {
            error = QString::fromStdString(nativeError);
            return false;
        }
        m_entryBinding = 0;
        m_activeEntryModule = 0;
        m_cube->setActivePatchLabel(QStringLiteral("builtin"));
        return true;
    }

    if (m_entryBinding != 0) {
        std::string nativeError;
        if (!m_patches.unbind(m_entryBinding, m_activeEntryModule, nativeError)) {
            error = QString::fromStdString(nativeError);
            return false;
        }
        m_entryBinding = 0;
    }

    m_activeEntryModule = 0;
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
            // can see that coming instead of being told after the fact.
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
