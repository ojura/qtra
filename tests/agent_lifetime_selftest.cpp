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
#include <QFile>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <dlfcn.h>
#include <sys/stat.h>
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

    std::printf("a reply sent after the client has gone\n");
    {
        // runtime_agent.h line 82 says a handler may answer later than it is
        // called. So a handler can hold what it needs to answer with across a
        // disconnect, and the client it names can be deleted in between:
        // removeClient calls deleteLater, and the deletion runs on a later turn
        // of the event loop, which is exactly where a deferred reply also runs.
        //
        // Holding a raw socket across that is a use-after-free the API invites.
        // Client keeps a QPointer, so the reply has something to find gone.
        QObject replyRoot;
        auto replying = std::make_unique<RuntimeAgent>(
            &replyRoot, QStringLiteral("/tmp/agent-lifetime-reply-%1.sock").arg(::getpid()));

        RuntimeAgent::Client held;
        QJsonValue heldId;
        bool handlerRan = false;
        const bool ok = replying->registerCommand(
            QStringLiteral("test.answerLater"), {},
            [&](RuntimeAgent::Client client, const QJsonValue& requestId,
                const QJsonObject&) {
                held = client;          // kept deliberately, which is the point
                heldId = requestId;
                handlerRan = true;
            });
        check(ok, "a command that answers later is registered");

        QString why;
        check(replying->start(why), why.isEmpty() ? "it listens" : qPrintable(why));

        {
            QLocalSocket caller;
            caller.connectToServer(replying->socketName());
            check(caller.waitForConnected(5000), "a caller connects");
            const QJsonObject ask{
                {QStringLiteral("id"), 7},
                {QStringLiteral("command"), QStringLiteral("test.answerLater")},
                {QStringLiteral("params"), QJsonObject{}},
            };
            caller.write(QJsonDocument(ask).toJson(QJsonDocument::Compact) + "\n");
            check(caller.waitForBytesWritten(5000), "and asks");

            const auto ranBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!handlerRan && std::chrono::steady_clock::now() < ranBy) {
                application.processEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            check(handlerRan, "the handler runs and keeps what it needs to answer");
            caller.disconnectFromServer();
        }

        // The socket is deleted here, not when disconnect was called. Draining
        // deferred deletes is what makes this the real case rather than a
        // simulation of it.
        for (int turn = 0; turn < 50; ++turn) {
            application.processEvents(QEventLoop::AllEvents, 5);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }

        // Answering now. There is nobody to answer, and the point is that this
        // returns rather than writing into freed storage.
        replying->sendSuccess(held, heldId, QJsonObject{{QStringLiteral("late"), true}});

        // Reaching this line at all is half the result, and under a sanitizer
        // it is the whole of it. The other half is that the agent is still
        // itself afterwards, which a fresh caller getting a real answer shows
        // and a bare "it did not crash" does not.
        handlerRan = false;
        QLocalSocket after;
        after.connectToServer(replying->socketName());
        check(after.waitForConnected(5000), "the agent still accepts a new caller");
        const QJsonObject again{
            {QStringLiteral("id"), 8},
            {QStringLiteral("command"), QStringLiteral("test.answerLater")},
            {QStringLiteral("params"), QJsonObject{}},
        };
        after.write(QJsonDocument(again).toJson(QJsonDocument::Compact) + "\n");
        check(after.waitForBytesWritten(5000), "and takes a request from it");
        const auto answeredBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!handlerRan && std::chrono::steady_clock::now() < answeredBy) {
            application.processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(handlerRan, "and dispatches it, so the late reply left it intact");
        after.disconnectFromServer();

        replying.reset();
    }

    std::printf("something already at the socket path\n");
    {
        // Starting used to clear whatever was in the way. A socket path is
        // ordinary filesystem namespace, so what is in the way can be a file
        // somebody wants, and an agent that deletes it to get its own listener
        // has destroyed data to start faster.
        const QString occupied =
            QStringLiteral("/tmp/agent-lifetime-occupied-%1").arg(::getpid());
        {
            QFile file(occupied);
            check(file.open(QIODevice::WriteOnly), "a regular file exists at the path");
            file.write("not a socket, and not the agent's to remove\n");
        }
        const QByteArray before = [&] {
            QFile file(occupied);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        }();
        check(!before.isEmpty(), "with contents to compare against");

        QObject anotherRoot;
        RuntimeAgent blocked(&anotherRoot, occupied);
        QString why;
        check(!blocked.start(why), "starting there is refused");
        check(!why.isEmpty(), why.isEmpty() ? "with a reason" : qPrintable(why));

        QFile after(occupied);
        check(after.exists(), "and the file is still there");
        if (after.open(QIODevice::ReadOnly)) {
            check(after.readAll() == before, "with its contents unchanged");
            after.close();
        }
        QFile::remove(occupied);
    }

    std::printf("the path replaced while the agent held it\n");
    {
        // Teardown removes the pathname it bound. Between listen and
        // destruction that path can be unlinked and something else put there,
        // and removing it then deletes a file the agent never owned. Unlike the
        // startup case there is nothing to refuse: from inside, deleting looks
        // like cleaning up after itself.
        const QString path =
            QStringLiteral("/tmp/agent-lifetime-replaced-%1.sock").arg(::getpid());
        QFile::remove(path);

        QObject thirdRoot;
        auto held = std::make_unique<RuntimeAgent>(&thirdRoot, path);
        QString why;
        check(held->start(why), why.isEmpty() ? "an agent binds the path" : qPrintable(why));

        // Who can reach it, asked of the socket rather than of the code.
        //
        // The listener chmods the node before listening, to keep the promise
        // README and docs/protocol.md both make about this being reachable only
        // by its own user. Every test here connects as that user, so a socket
        // opened to the world would pass all of them; the mode is the only
        // thing that says otherwise.
        {
            struct stat bound {};
            const QByteArray encoded = QFile::encodeName(path);
            check(::lstat(encoded.constData(), &bound) == 0, "the socket node is there");
            check(S_ISSOCK(bound.st_mode), "and is a socket");
            check((bound.st_mode & 0777) == 0600,
                  "reachable by its owner and nobody else");
        }

        // The socket goes, and an ordinary file takes its place.
        check(QFile::remove(path), "its socket is unlinked out from under it");
        const QByteArray planted = "somebody else's file, at a path the agent once used\n";
        {
            QFile file(path);
            check(file.open(QIODevice::WriteOnly), "and a regular file takes that name");
            file.write(planted);
        }

        held.reset();

        QFile after(path);
        check(after.exists(), "destroying the agent leaves the replacement alone");
        if (after.open(QIODevice::ReadOnly)) {
            check(after.readAll() == planted, "with its contents untouched");
            after.close();
        }
        QFile::remove(path);
    }

    qInstallMessageHandler(previousHandler);
    std::printf("%s\n", failures == 0 ? "all agent lifetime checks passed"
                                      : "agent lifetime checks failed");
    return failures == 0 ? 0 : 1;
}
