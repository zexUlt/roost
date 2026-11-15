#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <memory>

#include "core/protocol.hpp"
#include "roostd/backend.hpp"

using namespace roost;
using namespace std::chrono_literals;

namespace {

constexpr int TIMEOUT_MS = 5000;

template <typename TValue>
bool WaitFor(const QFuture<TValue>& future) {
    if (future.isFinished()) {
        return true;
    }
    QEventLoop loop;
    QFutureWatcher<TValue> watcher;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop,
                     &QEventLoop::quit);
    QTimer::singleShot(TIMEOUT_MS, &loop, &QEventLoop::quit);
    watcher.setFuture(future);
    loop.exec();
    return future.isFinished();
}

int CodeOf(const rpc::Result& result) {
    return result ? 0 : result.error().Code;
}

constexpr int UNAVAILABLE =
    static_cast<int>(protocol::EErrorCode::BackendUnavailable);

QJsonObject Command(const QString& command,
                    const QJsonValue& args = QJsonValue::Undefined) {
    QJsonObject params{{"command", command}};
    if (!args.isUndefined()) {
        params.insert("args", args);
    }
    return params;
}

// Small delays, so restarts happen within a test.
BackendOptions Fast() {
    return BackendOptions{
        .FirstRestartDelay = 10ms,
        .MaxRestartDelay = 30ms,
        .MaxRestarts = 5,
        .StableRun = 60s,
        .StopTimeout = 1s,
    };
}

bool IsAlive(qint64 pid) {
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
}

}  // namespace

class BackendTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<Backend> Make(const QStringList& arguments = {},
                                  BackendOptions options = Fast()) {
        return std::make_unique<Backend>(
            "fixture", QStringList{FIXTURE_BACKEND_PATH} + arguments,
            Dir_->path(), options);
    }

    rpc::Result Run(Backend& backend, const QString& command,
                    const QJsonValue& args = QJsonValue::Undefined) {
        const QFuture<rpc::Result> future =
            backend.Request("commands.run", Command(command, args));
        if (!WaitFor(future)) {
            return std::unexpected(rpc::Error{.Message = "timed out"});
        }
        return future.result();
    }

    std::unique_ptr<QTemporaryDir> Dir_;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
    }

    void AnswersARequest() {
        const auto backend = Make();
        backend->Start();
        const rpc::Result result =
            Run(*backend, "fixture.echo", QJsonArray{1, "two"});
        QVERIFY(result);
        QCOMPARE(*result, QJsonValue(QJsonArray{1, "two"}));
        QVERIFY(backend->IsRunning());
    }

    void RunsInItsDirectory() {
        const auto backend = Make();
        backend->Start();
        const rpc::Result result = Run(*backend, "fixture.cwd");
        QVERIFY(result);
        QCOMPARE(QDir(result->toString()).canonicalPath(),
                 QDir(Dir_->path()).canonicalPath());
    }

    void RunsInASessionOfItsOwn() {
        const auto backend = Make();
        backend->Start();
        const auto pid = static_cast<pid_t>(
            Run(*backend, "fixture.pid").value_or(0).toInteger());
        QVERIFY(pid > 0);
        QCOMPARE(::getsid(pid), pid);
        QCOMPARE(::getpgid(pid), pid);
    }

    void ReportsItsProcessId() {
        const auto backend = Make();
        backend->Start();
        const rpc::Result result = Run(*backend, "fixture.pid");
        QVERIFY(result);
        QCOMPARE(result->toInteger(), backend->ProcessId());
    }

    void PassesAnErrorThrough() {
        const auto backend = Make();
        backend->Start();
        const rpc::Result result = Run(*backend, "fixture.nope");
        QVERIFY(!result);
        QCOMPARE(result.error().Code, -32602);
        QCOMPARE(result.error().Message, "unknown command fixture.nope");
    }

    void AnswersEachRequestByItsId() {
        const auto backend = Make();
        backend->Start();
        QList<QFuture<rpc::Result>> futures;
        for (int i = 0; i < 20; ++i) {
            futures.append(backend->Request(
                "commands.run", Command("fixture.echo", QJsonArray{i})));
        }
        for (int i = 0; i < 20; ++i) {
            QVERIFY(WaitFor(futures[i]));
            QCOMPARE(futures[i].result().value(), QJsonValue(QJsonArray{i}));
        }
    }

    void JoinsALineSplitAcrossWrites() {
        const auto backend = Make();
        QSignalSpy crashed(backend.get(), &Backend::Crashed);
        backend->Start();
        // A notification comes first and is let through.
        const rpc::Result result = Run(*backend, "fixture.split");
        QVERIFY(result);
        QCOMPARE(*result, QJsonValue("split"));
        QCOMPARE(crashed.count(), 0);
    }

    void AnswersARequestFromTheBackend() {
        const auto backend = Make();
        backend->Start();
        const rpc::Result result = Run(*backend, "fixture.ask");
        QVERIFY(result);
        const QJsonObject reply = result->toObject();
        QCOMPARE(reply.value("id"), QJsonValue("ask"));
        QCOMPARE(reply.value("error").toObject().value("code").toInt(),
                 static_cast<int>(rpc::EErrorCode::MethodNotFound));
    }

    void FailsAtOnceWhenNotStarted() {
        const auto backend = Make();
        const QFuture<rpc::Result> future =
            backend->Request("commands.run", Command("fixture.echo"));
        QVERIFY(future.isFinished());
        QCOMPARE(CodeOf(future.result()), UNAVAILABLE);
    }

    void StartsOnce() {
        const auto backend = Make();
        QSignalSpy started(backend.get(), &Backend::Started);
        backend->Start();
        backend->Start();
        QVERIFY(Run(*backend, "fixture.echo"));
        backend->Start();
        QTest::qWait(50);
        QCOMPARE(started.count(), 1);
    }

    void RestartsAfterACrash() {
        const auto backend = Make();
        QSignalSpy started(backend.get(), &Backend::Started);
        QSignalSpy crashed(backend.get(), &Backend::Crashed);
        backend->Start();
        const qint64 firstPid = Run(*backend, "fixture.pid")->toInteger();

        const QFuture<rpc::Result> hanging =
            backend->Request("commands.run", Command("fixture.hang"));
        const rpc::Result crash = Run(*backend, "fixture.crash");
        QCOMPARE(CodeOf(crash), UNAVAILABLE);
        QVERIFY(WaitFor(hanging));
        QCOMPARE(CodeOf(hanging.result()), UNAVAILABLE);
        QVERIFY2(hanging.result().error().Message.contains(
                     "the backend of fixture exited with code 3"),
                 qPrintable(hanging.result().error().Message));

        QCOMPARE(crashed.count(), 1);
        QCOMPARE(crashed[0][0].toString(), "exited with code 3");
        QCOMPARE(crashed[0][1].toLongLong(), 10);
        QVERIFY(!backend->IsRunning());
        QCOMPARE(
            CodeOf(backend->Request("commands.run", Command("fixture.echo"))
                       .result()),
            UNAVAILABLE);

        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 2, TIMEOUT_MS);
        const rpc::Result pid = Run(*backend, "fixture.pid");
        QVERIFY(pid);
        QVERIFY(pid->toInteger() != firstPid);
        QCOMPARE(pid->toInteger(), backend->ProcessId());
    }

    void TreatsAKillAsACrash() {
        const auto backend = Make();
        QSignalSpy crashed(backend.get(), &Backend::Crashed);
        backend->Start();
        const qint64 pid = Run(*backend, "fixture.pid")->toInteger();
        QCOMPARE(::kill(static_cast<pid_t>(pid), SIGKILL), 0);
        QTRY_COMPARE_WITH_TIMEOUT(crashed.count(), 1, TIMEOUT_MS);
        QCOMPARE(crashed[0][0].toString(), "was killed by a signal");
    }

    void TreatsAnExitAsACrash() {
        // Its stdin is still open: exiting is its own idea.
        const auto backend = Make({"--crash-on-start"});
        QSignalSpy crashed(backend.get(), &Backend::Crashed);
        backend->Start();
        QTRY_VERIFY_WITH_TIMEOUT(crashed.count() >= 1, TIMEOUT_MS);
        QCOMPARE(crashed[0][0].toString(), "exited with code 3");
    }

    void KillsOneThatBreaksTheProtocol_data() {
        QTest::addColumn<QString>("command");
        QTest::addColumn<QString>("reason");
        QTest::newRow("not JSON")
            << "fixture.garbage"
            << "sent a line that is not a JSON-RPC message";
        QTest::newRow("a stray answer")
            << "fixture.stray" << "answered a request nobody made";
        QTest::newRow("a line too long")
            << "fixture.flood" << "sent a line longer than 16777216 bytes";
        QTest::newRow("an error that is not an object")
            << "fixture.badError" << "sent an error that is not an object";
    }

    void KillsOneThatBreaksTheProtocol() {
        QFETCH(QString, command);
        QFETCH(QString, reason);
        const auto backend = Make();
        QSignalSpy crashed(backend.get(), &Backend::Crashed);
        QSignalSpy started(backend.get(), &Backend::Started);
        backend->Start();

        const QFuture<rpc::Result> hanging =
            backend->Request("commands.run", Command("fixture.hang"));
        const rpc::Result result = Run(*backend, command);
        QCOMPARE(CodeOf(result), UNAVAILABLE);
        QCOMPARE(result.error().Message,
                 QString("the backend of fixture %1").arg(reason));
        QVERIFY(WaitFor(hanging));
        QCOMPARE(CodeOf(hanging.result()), UNAVAILABLE);
        QCOMPARE(crashed.count(), 1);
        QCOMPARE(crashed[0][0].toString(), reason);

        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 2, TIMEOUT_MS);
        QVERIFY(Run(*backend, "fixture.echo"));
    }

    void StartFromAContinuationWaitsForTheRestart() {
        QTest::failOnWarning(QRegularExpression("already running"));
        const auto backend = Make();
        QSignalSpy started(backend.get(), &Backend::Started);
        QSignalSpy crashed(backend.get(), &Backend::Crashed);
        backend->Start();
        Backend* raw = backend.get();
        // Without a context the continuation runs as the request fails.
        const QFuture<void> continued =
            backend->Request("commands.run", Command("fixture.crash"))
                .then([raw](const rpc::Result&) { raw->Start(); });

        QVERIFY(WaitFor(continued));
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 2, TIMEOUT_MS);
        QVERIFY(Run(*backend, "fixture.echo"));
        QCOMPARE(started.count(), 2);
        QCOMPARE(crashed.count(), 1);
    }

    void ReportsAFailedStart() {
        const auto backend = std::make_unique<Backend>(
            "fixture", QStringList{Dir_->filePath("missing")}, Dir_->path(),
            Fast());
        QSignalSpy crashed(backend.get(), &Backend::Crashed);
        backend->Start();
        QTRY_VERIFY_WITH_TIMEOUT(crashed.count() >= 1, TIMEOUT_MS);
        QVERIFY2(crashed[0][0].toString().startsWith("failed to start: "),
                 qPrintable(crashed[0][0].toString()));
    }

    void DoublesTheDelayThenGivesUp() {
        const auto backend = Make({"--crash-on-start"});
        QSignalSpy crashed(backend.get(), &Backend::Crashed);
        QSignalSpy gaveUp(backend.get(), &Backend::GaveUp);
        QSignalSpy started(backend.get(), &Backend::Started);
        backend->Start();
        QTRY_COMPARE_WITH_TIMEOUT(gaveUp.count(), 1, TIMEOUT_MS);
        QCOMPARE(gaveUp[0][0].toString(), "exited with code 3");

        QList<qint64> delays;
        for (const QList<QVariant>& signal : crashed) {
            delays.append(signal[1].toLongLong());
        }
        QCOMPARE(delays, (QList<qint64>{10, 20, 30, 30, 30}));
        QCOMPARE(started.count(), 6);

        // It stays down.
        QTest::qWait(100);
        QCOMPARE(started.count(), 6);
        QVERIFY(!backend->IsRunning());
        QCOMPARE(
            CodeOf(backend->Request("commands.run", Command("fixture.echo"))
                       .result()),
            UNAVAILABLE);
    }

    void StartsAgainAfterGivingUp() {
        BackendOptions options = Fast();
        options.MaxRestarts = 1;
        const auto backend = Make({}, options);
        QSignalSpy crashed(backend.get(), &Backend::Crashed);
        QSignalSpy gaveUp(backend.get(), &Backend::GaveUp);
        QSignalSpy started(backend.get(), &Backend::Started);
        backend->Start();
        QCOMPARE(CodeOf(Run(*backend, "fixture.crash")), UNAVAILABLE);
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 2, TIMEOUT_MS);
        QCOMPARE(CodeOf(Run(*backend, "fixture.crash")), UNAVAILABLE);
        QCOMPARE(gaveUp.count(), 1);

        // A new row of crashes begins.
        backend->Start();
        QVERIFY(Run(*backend, "fixture.echo"));
        QCOMPARE(CodeOf(Run(*backend, "fixture.crash")), UNAVAILABLE);
        QCOMPARE(crashed.count(), 2);
        QCOMPARE(gaveUp.count(), 1);
    }

    void StartWaitsOutARestartDelay() {
        BackendOptions options = Fast();
        options.FirstRestartDelay = 300ms;
        options.MaxRestartDelay = 300ms;
        options.MaxRestarts = 1;
        const auto backend = Make({}, options);
        QSignalSpy gaveUp(backend.get(), &Backend::GaveUp);
        QSignalSpy started(backend.get(), &Backend::Started);
        backend->Start();
        QCOMPARE(CodeOf(Run(*backend, "fixture.crash")), UNAVAILABLE);

        // Neither starts it early nor forgets the crash.
        backend->Start();
        QTest::qWait(100);
        QCOMPARE(started.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 2, TIMEOUT_MS);
        QCOMPARE(CodeOf(Run(*backend, "fixture.crash")), UNAVAILABLE);
        QCOMPARE(gaveUp.count(), 1);
    }

    void CountsCrashesInARow() {
        BackendOptions options = Fast();
        options.MaxRestarts = 1;
        options.StableRun = 300ms;
        const auto backend = Make({}, options);
        QSignalSpy crashed(backend.get(), &Backend::Crashed);
        QSignalSpy gaveUp(backend.get(), &Backend::GaveUp);
        QSignalSpy started(backend.get(), &Backend::Started);
        backend->Start();

        QCOMPARE(CodeOf(Run(*backend, "fixture.crash")), UNAVAILABLE);
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 2, TIMEOUT_MS);
        QCOMPARE(CodeOf(Run(*backend, "fixture.crash")), UNAVAILABLE);
        QCOMPARE(crashed.count(), 1);
        QCOMPARE(gaveUp.count(), 1);
    }

    void ForgetsCrashesAfterAStableRun() {
        BackendOptions options = Fast();
        options.MaxRestarts = 1;
        options.StableRun = 300ms;
        const auto backend = Make({}, options);
        QSignalSpy crashed(backend.get(), &Backend::Crashed);
        QSignalSpy gaveUp(backend.get(), &Backend::GaveUp);
        QSignalSpy started(backend.get(), &Backend::Started);
        backend->Start();

        QCOMPARE(CodeOf(Run(*backend, "fixture.crash")), UNAVAILABLE);
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 2, TIMEOUT_MS);
        QTest::qWait(400);
        QCOMPARE(CodeOf(Run(*backend, "fixture.crash")), UNAVAILABLE);
        QCOMPARE(crashed.count(), 2);
        // The row starts over, and so does the delay.
        QCOMPARE(crashed[1][1].toLongLong(), 10);
        QCOMPARE(gaveUp.count(), 0);
    }

    void StopsWhenDestroyed() {
        auto backend = Make();
        QSignalSpy crashed(backend.get(), &Backend::Crashed);
        backend->Start();
        const qint64 pid = Run(*backend, "fixture.pid")->toInteger();
        const QFuture<rpc::Result> hanging =
            backend->Request("commands.run", Command("fixture.hang"));

        QElapsedTimer timer;
        timer.start();
        backend.reset();
        // It exits on its own once its stdin closes.
        QVERIFY(timer.elapsed() < 900);
        QVERIFY(!IsAlive(pid));
        QVERIFY(hanging.isFinished());
        QCOMPARE(CodeOf(hanging.result()), UNAVAILABLE);
        QCOMPARE(crashed.count(), 0);
    }

    void IgnoresWhatItSaysWhileStopping() {
        QTest::failOnWarning(QRegularExpression("nobody made"));
        auto backend = Make();
        backend->Start();
        const QFuture<rpc::Result> last =
            backend->Request("commands.run", Command("fixture.lastWord"));
        // Answered in order, so the last word is now owed.
        QVERIFY(Run(*backend, "fixture.echo"));

        backend.reset();
        QVERIFY(last.isFinished());
        QCOMPARE(last.result().error().Message,
                 "roostd stopped the backend of fixture");
    }

    void KillsOneThatWillNotStop() {
        BackendOptions options = Fast();
        options.StopTimeout = 100ms;
        // Not left for QProcess to kill.
        QTest::failOnWarning(QRegularExpression("Destroyed while process"));
        auto backend = Make({"--linger-ms", "10000"}, options);
        backend->Start();
        const qint64 pid = Run(*backend, "fixture.pid")->toInteger();

        QElapsedTimer timer;
        timer.start();
        backend.reset();
        QVERIFY(timer.elapsed() < 2000);
        QVERIFY(!IsAlive(pid));
    }
};

QTEST_GUILESS_MAIN(BackendTest)
#include "backend_test.moc"
