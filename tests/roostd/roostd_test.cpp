#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>

#include <cerrno>
#include <memory>

#include "core/protocol.hpp"
#include "core/rpc_client.hpp"
#include "core/version.hpp"

using namespace roost;

namespace {

constexpr int TIMEOUT_MS = 10000;

// Whether a roostd, perhaps still starting, answers hello at the path.
bool Greets(const QString& socketPath) {
    const QDeadlineTimer deadline(TIMEOUT_MS);
    while (!deadline.hasExpired()) {
        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(socketPath);
        if (QTest::qWaitFor([&] { return hello.isFinished(); }, TIMEOUT_MS) &&
            hello.result()) {
            return true;
        }
        QTest::qWait(20);
    }
    return false;
}

// Under AddressSanitizer the fixture backend holds a pipe or a socket of its
// own, which no test here can tell from one handed down by roostd.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool UNDER_ASAN = true;
#else
constexpr bool UNDER_ASAN = false;
#endif
#else
constexpr bool UNDER_ASAN = false;
#endif

rpc::Result Call(RpcClient& client, const QString& method,
                 const QJsonObject& params) {
    QFuture<rpc::Result> future = client.Request(method, params);
    if (!QTest::qWaitFor([&] { return future.isFinished(); }, TIMEOUT_MS)) {
        return std::unexpected(rpc::Error{.Message = "timed out"});
    }
    return future.result();
}

// The pipes and sockets of this process that any child of it inherits, as
// fixture.fds lists them.
QJsonArray InheritedDescriptors() {
    QJsonArray inherited;
    for (int fd = 3; fd < 1024; ++fd) {
        const int flags = ::fcntl(fd, F_GETFD);
        struct stat status{};
        if (flags != -1 && (flags & FD_CLOEXEC) == 0 &&
            ::fstat(fd, &status) == 0 &&
            (S_ISFIFO(status.st_mode) || S_ISSOCK(status.st_mode))) {
            inherited.append(fd);
        }
    }
    return inherited;
}

}  // namespace

// roostd as it runs: a process that finds its socket through the environment.
class RoostdTest : public QObject {
    Q_OBJECT

private:
    QString SocketPath() const { return Dir_->filePath("roostd.sock"); }

    std::unique_ptr<QProcess> Launch(const QStringList& arguments = {}) const {
        auto process = std::make_unique<QProcess>();
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("ROOST_SOCKET", SocketPath());
        environment.insert("ROOST_CONFIG", Dir_->filePath("config"));
        environment.insert("ROOST_PLUGINS_PATH", Dir_->filePath("plugins"));
        process->setProcessEnvironment(environment);
        process->start(ROOSTD_PATH, arguments);
        return process;
    }

    // A plugin "fixture" whose backend is the fixture backend.
    bool AddFixturePlugin() const {
        const QDir plugin(Dir_->filePath("plugins/fixture"));
        QFile manifest(plugin.filePath("plugin.json"));
        if (!plugin.mkpath(".") ||
            !QFile::link(FIXTURE_BACKEND_PATH, plugin.filePath("backend")) ||
            !manifest.open(QIODevice::WriteOnly)) {
            return false;
        }
        return manifest.write(R"({
            "id": "fixture", "version": "1", "api": "0",
            "backend": {"command": ["backend"]},
            "contributes": {"commands": [
                {"id": "fixture.echo", "title": "Echo"},
                {"id": "fixture.pid", "title": "Process id"},
                {"id": "fixture.fds", "title": "Open descriptors"},
                {"id": "fixture.crash", "title": "Crash"}
            ]}
        })") > 0;
    }

    std::unique_ptr<QTemporaryDir> Dir_;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
    }

    void ServesItsSocket() {
        const auto daemon = Launch();
        QVERIFY(daemon->waitForStarted(TIMEOUT_MS));
        // The socket appears once it listens: Qt binds it aside and moves it
        // into place.
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(SocketPath()), TIMEOUT_MS);

        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(SocketPath());
        QVERIFY(
            QTest::qWaitFor([&] { return hello.isFinished(); }, TIMEOUT_MS));
        QVERIFY(hello.result());
        QCOMPARE(hello.result()->toObject().value("roostd").toString(),
                 QString::fromStdString(VersionString()));

        daemon->terminate();
        QVERIFY(daemon->waitForFinished(TIMEOUT_MS));
    }

    void SecondDaemonExits() {
        const auto first = Launch();
        QVERIFY(first->waitForStarted(TIMEOUT_MS));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(SocketPath()), TIMEOUT_MS);

        const auto second = Launch();
        QVERIFY(second->waitForFinished(TIMEOUT_MS));
        QCOMPARE(second->exitStatus(), QProcess::NormalExit);
        QCOMPARE(second->exitCode(), 1);
        const QString error = QString::fromUtf8(second->readAllStandardError());
        QVERIFY2(error.contains("another roostd"), qPrintable(error));

        first->terminate();
        QVERIFY(first->waitForFinished(TIMEOUT_MS));
    }

    void StopsCleanlyOnASignal_data() {
        QTest::addColumn<int>("signal");
        QTest::newRow("SIGTERM") << SIGTERM;
        QTest::newRow("SIGINT") << SIGINT;
    }

    void StopsCleanlyOnASignal() {
        QFETCH(int, signal);
        const auto daemon = Launch();
        QVERIFY(daemon->waitForStarted(TIMEOUT_MS));
        QVERIFY(Greets(SocketPath()));
        QCOMPARE(::kill(static_cast<pid_t>(daemon->processId()), signal), 0);
        QVERIFY(daemon->waitForFinished(TIMEOUT_MS));
        QCOMPARE(daemon->exitStatus(), QProcess::NormalExit);
        QCOMPARE(daemon->exitCode(), 0);
        QVERIFY(!QFile::exists(SocketPath()));
        QVERIFY(!QFile::exists(SocketPath() + ".lock"));
    }

    // The M1 criterion: a client runs a plugin command through the socket,
    // and a crashed backend comes back after an event about the crash.
    void RunsAPluginCommandAndReportsItsCrash() {
        QVERIFY(AddFixturePlugin());
        const auto daemon = Launch();
        QVERIFY(daemon->waitForStarted(TIMEOUT_MS));
        QVERIFY(Greets(SocketPath()));
        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(SocketPath());
        QVERIFY(
            QTest::qWaitFor([&] { return hello.isFinished(); }, TIMEOUT_MS));
        QVERIFY(hello.result());
        QSignalSpy notified(&client, &RpcClient::NotificationReceived);
        const auto call = [&](const QString& method,
                              const QJsonObject& params) {
            return Call(client, method, params);
        };
        QVERIFY(call("bus.subscribe", {{"pattern", "plugin.*"}}));

        const rpc::Result echoed = call(
            "commands.run", {{"command", "fixture.echo"}, {"args", "hello"}});
        QVERIFY(echoed);
        QCOMPARE(*echoed, QJsonValue("hello"));
        const qint64 pid = call("commands.run", {{"command", "fixture.pid"}})
                               .value_or(QJsonValue())
                               .toInteger();
        QVERIFY(pid > 0);

        const rpc::Result crashed =
            call("commands.run", {{"command", "fixture.crash"}});
        QVERIFY(!crashed);
        QCOMPARE(crashed.error().Code,
                 static_cast<int>(protocol::EErrorCode::BackendUnavailable));
        QTRY_COMPARE_WITH_TIMEOUT(notified.count(), 1, TIMEOUT_MS);
        QCOMPARE(notified[0][0].toString(), "bus.event");
        const QJsonObject event = notified[0][1].value<QJsonValue>().toObject();
        QCOMPARE(event.value("name"), QJsonValue("plugin.backendCrashed"));
        QCOMPARE(event.value("payload"),
                 QJsonValue(QJsonObject{{"plugin", "fixture"},
                                        {"reason", "exited with code 3"},
                                        {"restartInMs", 500}}));

        // Back after the delay, as another process.
        QTRY_VERIFY_WITH_TIMEOUT(
            call("commands.run", {{"command", "fixture.echo"}}).has_value(),
            TIMEOUT_MS);
        const qint64 restarted =
            call("commands.run", {{"command", "fixture.pid"}})
                .value_or(QJsonValue())
                .toInteger();
        QVERIFY(restarted > 0 && restarted != pid);

        daemon->terminate();
        QVERIFY(daemon->waitForFinished(TIMEOUT_MS));
        QCOMPARE(daemon->exitCode(), 0);
        // The backend went with it.
        QVERIFY(::kill(static_cast<pid_t>(restarted), 0) == -1 &&
                errno == ESRCH);
    }

    void BackendsInheritNoPipeOfRoostd() {
        if constexpr (UNDER_ASAN) {
            QSKIP("the fixture backend holds a descriptor of its own");
        }
        QVERIFY(AddFixturePlugin());
        const auto daemon = Launch();
        QVERIFY(daemon->waitForStarted(TIMEOUT_MS));
        QVERIFY(Greets(SocketPath()));
        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(SocketPath());
        QVERIFY(
            QTest::qWaitFor([&] { return hello.isFinished(); }, TIMEOUT_MS));
        QVERIFY(hello.result());

        // Nothing of roostd's own, such as its socket or its signal pipe.
        const rpc::Result fds =
            Call(client, "commands.run", {{"command", "fixture.fds"}});
        QVERIFY(fds);
        QCOMPARE(*fds, QJsonValue(InheritedDescriptors()));

        daemon->terminate();
        QVERIFY(daemon->waitForFinished(TIMEOUT_MS));
    }

    void DumpPrintsWhatRoostdSends() {
        const auto daemon = Launch();
        QVERIFY(daemon->waitForStarted(TIMEOUT_MS));
        QVERIFY(Greets(SocketPath()));
        const auto dump = Launch({"--dump"});
        QVERIFY(dump->waitForStarted(TIMEOUT_MS));
        QByteArray printed;
        // The answers to its hello and its subscription.
        QTRY_VERIFY_WITH_TIMEOUT(
            (printed += dump->readAllStandardOutput()).count('\n') >= 2,
            TIMEOUT_MS);
        QVERIFY2(printed.startsWith("rpc {\"id\":1,\"jsonrpc\":\"2.0\","
                                    "\"result\":{"),
                 printed.constData());

        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(SocketPath());
        QVERIFY(
            QTest::qWaitFor([&] { return hello.isFinished(); }, TIMEOUT_MS));
        QVERIFY(hello.result());
        client.Request("bus.publish",
                       QJsonObject{{"name", "test.dumped"}, {"payload", 42}});
        QTRY_VERIFY_WITH_TIMEOUT(
            (printed += dump->readAllStandardOutput())
                .contains(R"("name":"test.dumped","payload":42)"),
            TIMEOUT_MS);
        QVERIFY2(printed.contains("\nrpc {\"jsonrpc\":\"2.0\",\"method\":"
                                  "\"bus.event\""),
                 printed.constData());

        // It ends with the daemon.
        daemon->terminate();
        QVERIFY(daemon->waitForFinished(TIMEOUT_MS));
        QVERIFY(dump->waitForFinished(TIMEOUT_MS));
        QCOMPARE(dump->exitStatus(), QProcess::NormalExit);
        QCOMPARE(dump->exitCode(), 0);
    }

    void DumpWithoutADaemonFails() {
        const auto dump = Launch({"--dump"});
        QVERIFY(dump->waitForFinished(TIMEOUT_MS));
        QCOMPARE(dump->exitCode(), 1);
        const QString error = QString::fromUtf8(dump->readAllStandardError());
        QVERIFY2(error.startsWith("roostd --dump: cannot connect to "),
                 qPrintable(error));
    }

    void KilledDaemonIsReplaced() {
        const auto killed = Launch();
        QVERIFY(killed->waitForStarted(TIMEOUT_MS));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(SocketPath()), TIMEOUT_MS);
        killed->kill();
        QVERIFY(killed->waitForFinished(TIMEOUT_MS));
        // Its socket and lock are still there.
        QVERIFY(QFile::exists(SocketPath()));

        const auto next = Launch();
        QVERIFY(next->waitForStarted(TIMEOUT_MS));
        QVERIFY(Greets(SocketPath()));
        QCOMPARE(next->state(), QProcess::Running);
        next->terminate();
        QVERIFY(next->waitForFinished(TIMEOUT_MS));
    }
};

QTEST_GUILESS_MAIN(RoostdTest)
#include "roostd_test.moc"
