#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <memory>
#include <optional>

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

    // `workingDirectory` and `closeOutput` (stdout and stderr closed before
    // exec) are for the tests that need them.
    std::unique_ptr<QProcess> Launch(const QStringList& arguments = {},
                                     const QString& workingDirectory = {},
                                     bool closeOutput = false) const {
        auto process = std::make_unique<QProcess>();
        if (!workingDirectory.isEmpty()) {
            process->setWorkingDirectory(workingDirectory);
        }
        if (closeOutput) {
            process->setChildProcessModifier([] {
                ::close(STDOUT_FILENO);
                ::close(STDERR_FILENO);
            });
        }
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("ROOST_SOCKET", SocketPath());
        environment.insert("ROOST_CONFIG", Dir_->filePath("config"));
        environment.insert("ROOST_PLUGINS_PATH", Dir_->filePath("plugins"));
        process->setProcessEnvironment(environment);
        process->start(ROOSTD_PATH, arguments);
        return process;
    }

    QString LogPath() const { return Dir_->filePath("roostd.log"); }

    static QByteArray ReadFile(const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    static bool WriteFile(const QString& path, const QByteArray& content) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(content) > 0;
    }

    static ino_t InodeOf(const QString& path) {
        struct stat status{};
        return ::stat(QFile::encodeName(path).constData(), &status) == 0
                   ? status.st_ino
                   : 0;
    }

    // roostd as the package has it: a copy in <dir>/bin, so that
    // <dir>/roost-plugins is its bundled directory, with no
    // ROOST_PLUGINS_PATH. The state is in the temporary directory, as in
    // Launch().
    std::unique_ptr<QProcess> LaunchCopy() const {
        const QString copy = Dir_->filePath("bin/roostd");
        if (!QDir(Dir_->path()).mkpath("bin") ||
            !QFile::copy(ROOSTD_PATH, copy)) {
            return nullptr;
        }
        auto process = std::make_unique<QProcess>();
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("ROOST_SOCKET", SocketPath());
        environment.insert("ROOST_CONFIG", Dir_->filePath("config"));
        environment.remove("ROOST_PLUGINS_PATH");
        process->setProcessEnvironment(environment);
        process->start(copy, {});
        return process;
    }

    // The answer of plugins.list of the roostd at the socket.
    std::optional<QJsonObject> ListPlugins() const {
        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(SocketPath());
        if (!QTest::qWaitFor([&] { return hello.isFinished(); }, TIMEOUT_MS) ||
            !hello.result()) {
            return std::nullopt;
        }
        const rpc::Result answer = Call(client, "plugins.list", QJsonObject());
        if (!answer) {
            return std::nullopt;
        }
        return answer->toObject();
    }

    static QStringList PluginIds(const QJsonObject& list) {
        QStringList ids;
        for (const QJsonValue plugin : list.value("plugins").toArray()) {
            ids.append(plugin.toObject().value("id").toString());
        }
        return ids;
    }

    // A plugin "fixture" whose backend is the fixture backend, in
    // <Dir_>/<directory>.
    bool AddFixturePlugin(const QString& directory = "plugins") const {
        const QDir plugin(Dir_->filePath(directory + "/fixture"));
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
                {"id": "fixture.stderr", "title": "Write to stderr"},
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

    // The M5 criterion in small: the built-in plugins are found next to the
    // binary, with nothing in the environment.
    void FindsTheBundledPluginsWithoutTheVariable() {
        QVERIFY(AddFixturePlugin("roost-plugins"));
        const auto daemon = LaunchCopy();
        QVERIFY(daemon);
        QVERIFY(daemon->waitForStarted(TIMEOUT_MS));
        QVERIFY(Greets(SocketPath()));
        const auto list = ListPlugins();
        QVERIFY(list);
        QCOMPARE(PluginIds(*list), QStringList{"fixture"});
        QCOMPARE(list->value("problems").toArray(), QJsonArray());

        daemon->terminate();
        QVERIFY(daemon->waitForFinished(TIMEOUT_MS));
    }

    // The bundled directory is searched before <config>/plugins, so a plugin
    // of the user with a built-in id is the one that is not loaded.
    void BuiltInPluginWinsOverOneOfTheUserWithTheSameId() {
        QVERIFY(AddFixturePlugin("roost-plugins"));
        QVERIFY(AddFixturePlugin("config/plugins"));
        const auto daemon = LaunchCopy();
        QVERIFY(daemon);
        QVERIFY(daemon->waitForStarted(TIMEOUT_MS));
        QVERIFY(Greets(SocketPath()));
        const auto list = ListPlugins();
        QVERIFY(list);
        QCOMPARE(PluginIds(*list), QStringList{"fixture"});
        const QJsonArray problems = list->value("problems").toArray();
        QCOMPARE(problems.size(), 1);
        QVERIFY2(problems.at(0).toString().contains(
                     "in " + Dir_->filePath("config/plugins/fixture") +
                     " is not loaded"),
                 qPrintable(problems.at(0).toString()));

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

    void LogFileTakesOutputOfRoostdAndBackends() {
        QVERIFY(AddFixturePlugin());
        const QString log = LogPath();
        const auto daemon = Launch({"--log", log});
        QVERIFY(daemon->waitForStarted(TIMEOUT_MS));
        QVERIFY(Greets(SocketPath()));
        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(SocketPath());
        QVERIFY(
            QTest::qWaitFor([&] { return hello.isFinished(); }, TIMEOUT_MS));
        QVERIFY(hello.result());
        QVERIFY(Call(
            client, "commands.run",
            {{"command", "fixture.stderr"}, {"args", "from the backend"}}));

        // roostd's own line and the backend's stderr, nothing on the pipes.
        QTRY_VERIFY_WITH_TIMEOUT(
            ReadFile(log).contains("roostd: listening on ") &&
                ReadFile(log).contains("from the backend\n"),
            TIMEOUT_MS);
        QVERIFY(daemon->readAllStandardOutput().isEmpty());
        QVERIFY(daemon->readAllStandardError().isEmpty());
        QCOMPARE(QFile::permissions(log) &
                     (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                      QFileDevice::ReadOther | QFileDevice::WriteOther),
                 QFileDevice::Permissions());
        QVERIFY(!QFile::exists(log + ".1"));

        daemon->terminate();
        QVERIFY(daemon->waitForFinished(TIMEOUT_MS));
    }

    void LogFileOfThePreviousRunIsKept() {
        const QString log = LogPath();
        const auto first = Launch({"--log", log});
        QVERIFY(first->waitForStarted(TIMEOUT_MS));
        QVERIFY(Greets(SocketPath()));
        first->terminate();
        QVERIFY(first->waitForFinished(TIMEOUT_MS));
        const QByteArray firstRun = ReadFile(log);
        QVERIFY(firstRun.contains("roostd: listening on "));
        // An older .1 goes away.
        QVERIFY(WriteFile(log + ".1", "stale copy\n"));

        const auto second = Launch({"--log", log});
        QVERIFY(second->waitForStarted(TIMEOUT_MS));
        QVERIFY(Greets(SocketPath()));
        QCOMPARE(ReadFile(log + ".1"), firstRun);
        QTRY_VERIFY_WITH_TIMEOUT(
            ReadFile(log).contains("roostd: listening on "), TIMEOUT_MS);
        QVERIFY(!ReadFile(log).contains("stale copy"));
        second->terminate();
        QVERIFY(second->waitForFinished(TIMEOUT_MS));
    }

    void SecondDaemonLeavesTheLogAlone() {
        const QString log = LogPath();
        const auto first = Launch({"--log", log});
        QVERIFY(first->waitForStarted(TIMEOUT_MS));
        QVERIFY(Greets(SocketPath()));
        QTRY_VERIFY_WITH_TIMEOUT(
            ReadFile(log).contains("roostd: listening on "), TIMEOUT_MS);
        const QByteArray content = ReadFile(log);
        const ino_t inode = InodeOf(log);
        QVERIFY(inode != 0);

        const auto second = Launch({"--log", log});
        QVERIFY(second->waitForFinished(TIMEOUT_MS));
        QCOMPARE(second->exitStatus(), QProcess::NormalExit);
        QCOMPARE(second->exitCode(), 1);
        // The error stays on its stderr, not in the log.
        const QString error = QString::fromUtf8(second->readAllStandardError());
        QVERIFY2(error.contains("another roostd"), qPrintable(error));

        QCOMPARE(InodeOf(log), inode);
        QVERIFY(ReadFile(log).startsWith(content));
        QVERIFY(!ReadFile(log).contains("another roostd"));
        QVERIFY(!QFile::exists(log + ".1"));
        first->terminate();
        QVERIFY(first->waitForFinished(TIMEOUT_MS));
    }

    void WithoutLogOptionOutputStaysOnItsStreams() {
        const auto daemon = Launch();
        QVERIFY(daemon->waitForStarted(TIMEOUT_MS));
        QVERIFY(Greets(SocketPath()));
        QByteArray error;
        QTRY_VERIFY_WITH_TIMEOUT((error += daemon->readAllStandardError())
                                     .contains("roostd: listening on "),
                                 TIMEOUT_MS);
        daemon->terminate();
        QVERIFY(daemon->waitForFinished(TIMEOUT_MS));
        QVERIFY(!QFile::exists(LogPath()));
        QVERIFY(QDir(Dir_->path()).entryList({"*.log*"}).isEmpty());
    }

    void LogFileWithClosedStdioStaysSmall() {
        const QString log = LogPath();
        const auto daemon = Launch({"--log", log}, {}, true);
        QVERIFY(daemon->waitForStarted(TIMEOUT_MS));
        QVERIFY(Greets(SocketPath()));
        QTRY_VERIFY_WITH_TIMEOUT(
            ReadFile(log).contains("roostd: listening on "), TIMEOUT_MS);
        // Without the descriptors reopened, Qt's wake pipe sits on 1 or 2 and
        // the log fills with "QThreadPipe: internal error" in no time.
        QTest::qWait(1000);
        QCOMPARE(daemon->state(), QProcess::Running);
        QVERIFY2(QFileInfo(log).size() < 4096,
                 qPrintable(QString::number(QFileInfo(log).size())));
        QVERIFY(!ReadFile(log).contains("QThreadPipe"));
        QVERIFY(Greets(SocketPath()));
        daemon->terminate();
        QVERIFY(daemon->waitForFinished(TIMEOUT_MS));
    }

    void LogFileGivenRelativelyLandsInTheWorkingDirectory() {
        QVERIFY(QDir(Dir_->path()).mkpath("work"));
        const QString work = Dir_->filePath("work");
        const auto daemon = Launch({"--log", "rel.log"}, work);
        QVERIFY(daemon->waitForStarted(TIMEOUT_MS));
        QVERIFY(Greets(SocketPath()));
        QTRY_VERIFY_WITH_TIMEOUT(
            ReadFile(work + "/rel.log").contains("roostd: listening on "),
            TIMEOUT_MS);
        daemon->terminate();
        QVERIFY(daemon->waitForFinished(TIMEOUT_MS));
    }

    void LogFileInAMissingDirectoryStopsRoostd() {
        const auto daemon =
            Launch({"--log", Dir_->filePath("none/roostd.log")});
        QVERIFY(daemon->waitForFinished(TIMEOUT_MS));
        QCOMPARE(daemon->exitStatus(), QProcess::NormalExit);
        QCOMPARE(daemon->exitCode(), 1);
        const QString error = QString::fromUtf8(daemon->readAllStandardError());
        QVERIFY2(error.contains("cannot open"), qPrintable(error));
        QVERIFY(!QFile::exists(SocketPath()));
    }

    void FailedLogOpenKeepsThePreviousLogs() {
        const QString log = LogPath();
        QVERIFY(WriteFile(log, "current\n"));
        QVERIFY(WriteFile(log + ".1", "older\n"));
        // A directory where the new file goes: the open fails.
        QVERIFY(QDir(Dir_->path()).mkpath("roostd.log.new"));
        const auto daemon = Launch({"--log", log});
        QVERIFY(daemon->waitForFinished(TIMEOUT_MS));
        QCOMPARE(daemon->exitCode(), 1);
        QVERIFY(QString::fromUtf8(daemon->readAllStandardError())
                    .contains("cannot open"));
        QCOMPARE(ReadFile(log), QByteArray("current\n"));
        QCOMPARE(ReadFile(log + ".1"), QByteArray("older\n"));
        QVERIFY(!QFile::exists(SocketPath()));
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
