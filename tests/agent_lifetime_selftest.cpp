// What destroying the agent does to module code that is still running.
//
// An executor runs a snippet wherever the application says, including a thread
// of its own. So a snippet can be inside its run entry point at the moment the
// agent is destroyed, holding a host whose callbacks reach agent state. Freeing
// that state under running module code is worse than waiting for it, so
// destruction waits.
//
// Its own binary. It destroys an agent and races threads against that, and a
// process that has done so is not one other tests should share.

#include "agent/module_registry.h"
#include "agent/runtime_agent.h"

#include <QCoreApplication>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <dlfcn.h>
#include <memory>
#include <string>
#include <thread>

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

// What the module exposes so this test can drive the race.
using BoolEntry = bool (*)();
using VoidEntry = void (*)();

// The process log, captured so the call made after teardown can be seen to have
// reached it.
std::atomic<int> logLinesAfterTeardown{0};
std::atomic<bool> countingLog{false};
QtMessageHandler previousHandler = nullptr;

void captureMessages(QtMsgType type, const QMessageLogContext& context, const QString& text)
{
    if (countingLog.load(std::memory_order_acquire)
        && text.contains(QLatin1String("calling after teardown"))) {
        logLinesAfterTeardown.fetch_add(1, std::memory_order_release);
    }
    if (previousHandler != nullptr) {
        previousHandler(type, context, text);
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc < 2) {
        std::printf("usage: agent_lifetime_selftest <lifetime module>\n");
        return 2;
    }
    const QString modulePath = QString::fromLocal8Bit(argv[1]);

    void* handle = ::dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        std::printf("  FAIL the module could not be opened: %s\n", ::dlerror());
        return 1;
    }
    auto isInside = reinterpret_cast<BoolEntry>(::dlsym(handle, "lifetimeModuleIsInside"));
    auto release = reinterpret_cast<VoidEntry>(::dlsym(handle, "lifetimeModuleRelease"));
    auto finished = reinterpret_cast<BoolEntry>(::dlsym(handle, "lifetimeModuleFinished"));
    auto callAfter =
        reinterpret_cast<BoolEntry>(::dlsym(handle, "lifetimeModuleCallAfterwards"));
    if (isInside == nullptr || release == nullptr || finished == nullptr
        || callAfter == nullptr) {
        std::printf("  FAIL the module does not expose what this test drives\n");
        return 1;
    }

    previousHandler = qInstallMessageHandler(&captureMessages);

    std::printf("destroying the agent while module code is running\n");

    QObject root;
    auto agent = std::make_unique<RuntimeAgent>(
        &root, QStringLiteral("/tmp/agent-lifetime-selftest-%1.sock").arg(::getpid()));

    // Its own thread, which is the case this exists for: the callback does not
    // run on the agent's thread, so it can still be inside the module when the
    // agent is destroyed.
    std::thread worker;
    const bool registered = agent->registerExecutor(
        QStringLiteral("worker"), [&worker](std::function<void()> call) {
            if (worker.joinable()) {
                return false;
            }
            worker = std::thread(std::move(call));
            return true;
        });
    check(registered, "an executor backed by its own thread is registered");

    QString error;
    check(agent->start(error), error.isEmpty() ? "the agent is listening" : qPrintable(error));

    runtime_agent::ModuleRegistry::LoadedModule* module =
        runtime_agent::ModuleRegistry::instance().loadSnippet(modulePath, error);
    check(module != nullptr,
          module != nullptr ? "the lifetime module loads" : qPrintable(error));
    if (module == nullptr) {
        return 1;
    }

    // Driven over the socket, because that is the path an application's snippet
    // actually takes.
    QLocalSocket client;
    client.connectToServer(agent->socketName());
    check(client.waitForConnected(5000), "a client connects");

    const QJsonObject request{
        {QStringLiteral("id"), 1},
        {QStringLiteral("command"), QStringLiteral("snippet.run")},
        {QStringLiteral("params"),
         QJsonObject{{QStringLiteral("moduleId"), QString::number(module->id)},
                     {QStringLiteral("executor"), QStringLiteral("worker")},
                     {QStringLiteral("request"), QJsonObject{}}}},
    };
    client.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n");
    check(client.waitForBytesWritten(5000), "the run request is sent");

    // The agent schedules on its own thread, so let it.
    const auto enteredBy = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!isInside() && std::chrono::steady_clock::now() < enteredBy) {
        application.processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(isInside(), "the module is inside its run entry point");

    // The race, in the order it has to happen: destruction starts and blocks,
    // this releases the module, destruction returns.
    std::atomic<bool> destructionStarted{false};
    std::atomic<bool> destructionReturned{false};
    std::atomic<bool> stillBlockedWhileInside{false};

    std::thread releaser([&] {
        while (!destructionStarted.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Long enough that a destructor which does not wait would have
        // returned, and short enough not to stall the run.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stillBlockedWhileInside.store(!destructionReturned.load(std::memory_order_acquire),
                                      std::memory_order_release);
        release();
    });

    destructionStarted.store(true, std::memory_order_release);
    agent.reset();
    destructionReturned.store(true, std::memory_order_release);
    releaser.join();
    if (worker.joinable()) {
        worker.join();
    }

    check(stillBlockedWhileInside.load(std::memory_order_acquire),
          "destruction waits while module code is on the stack");
    check(finished(), "and the module's run entry point ran to the end");

    std::printf("a host kept by the module, used after the agent is gone\n");
    {
        countingLog.store(true, std::memory_order_release);
        const bool called = callAfter();
        check(called, "the saved host is still callable");
        check(logLinesAfterTeardown.load(std::memory_order_acquire) > 0,
              "and its log reaches the process log, which needs no live agent");
    }

    qInstallMessageHandler(previousHandler);
    std::printf("%s\n", failures == 0 ? "all agent lifetime checks passed"
                                      : "agent lifetime checks failed");
    return failures == 0 ? 0 : 1;
}
