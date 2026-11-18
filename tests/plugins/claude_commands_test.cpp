// The commands of the Claude plugin (stop, remove, respawn, respawn all): the
// real claude-adapter binary over stdio with a fake `claude` first on PATH,
// and the manifest of the built plugin through the real roostd. The fake
// never touches a real Claude account.

#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <signal.h>

#include <memory>
#include <optional>

#include "core/rpc_client.hpp"

using namespace roost;

namespace {

constexpr int TIMEOUT_MS = 5000;

// A fake `claude`, steered by files next to it. For a verb (`agents`, `stop`,
// `rm`, `respawn`), a file named <verb>.<what>:
//   exit    its exit code
//   stdout  what it prints
//   stderr  what it prints to stderr
//   sleep   seconds to take
//   hang    if present, writes the pids of itself and of a child of its group
//           to pids and sleeps for good
//   hold    if present, waits until the file is removed, after the listing
//           of `agents` is read and before it prints
//   agents  replaces agents.json whole before it exits, as a removal would
// `agents` prints agents.json as it was when the run started. Each run adds
// "start <args>" and "end" lines to calls.
constexpr const char* FAKE_CLAUDE = R"sh(#!/bin/sh
dir=$(dirname "$0")
verb=$1
echo "start $*" >> "$dir/calls"
if [ "$verb" = agents ]; then listing=$(cat "$dir/agents.json"); fi
if [ -f "$dir/$verb.hang" ]; then
  sleep 600 & echo $! >> "$dir/pids"; echo $$ >> "$dir/pids"; exec sleep 600
fi
while [ -f "$dir/$verb.hold" ]; do sleep 0.05; done
if [ -f "$dir/$verb.sleep" ]; then sleep "$(cat "$dir/$verb.sleep")"; fi
if [ -f "$dir/$verb.agents" ]; then
  cp "$dir/$verb.agents" "$dir/agents.tmp" && mv "$dir/agents.tmp" "$dir/agents.json"
fi
if [ -f "$dir/$verb.stderr" ]; then cat "$dir/$verb.stderr" >&2; fi
if [ -f "$dir/$verb.stdout" ]; then cat "$dir/$verb.stdout"; fi
if [ "$verb" = agents ]; then echo "$listing"; fi
echo end >> "$dir/calls"
exit "$(cat "$dir/$verb.exit" 2>/dev/null || echo 0)"
)sh";

QByteArray Agents(const QJsonArray& entries) {
    return QJsonDocument(entries).toJson(QJsonDocument::Compact);
}

QJsonObject Entry(const QString& id, const QString& state) {
    return {{"id", id}, {"kind", "background"}, {"state", state}};
}

QJsonObject Request(qint64 id, const QString& method,
                    const QJsonObject& params) {
    return {
        {"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}

QJsonObject Run(const QString& command, const QJsonValue& args) {
    QJsonObject params{{"command", command}};
    if (!args.isUndefined()) {
        params.insert("args", args);
    }
    return params;
}

bool WaitFor(const QFuture<rpc::Result>& future) {
    if (future.isFinished()) {
        return true;
    }
    QEventLoop loop;
    QFutureWatcher<rpc::Result> watcher;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop,
                     &QEventLoop::quit);
    QTimer::singleShot(TIMEOUT_MS, &loop, &QEventLoop::quit);
    watcher.setFuture(future);
    loop.exec();
    return future.isFinished();
}

int ErrorCode(const QJsonObject& response) {
    return response.value("error").toObject().value("code").toInt();
}

}  // namespace

class ClaudeCommandsTest : public QObject {
    Q_OBJECT

private:
    QString FakePath(const QString& name) const {
        return Dir_->filePath("bin/" + name);
    }

    // Replaces the file whole, so that no run reads half of it.
    void Put(const QString& name, const QByteArray& content) {
        QSaveFile file(FakePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(content), content.size());
        QVERIFY(file.commit());
    }

    QStringList AllCalls() const {
        QFile file(FakePath("calls"));
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QString::fromUtf8(file.readAll())
            .split(u'\n', Qt::SkipEmptyParts);
    }

    // The starts of commands: no `agents` runs, no ends.
    QStringList CommandCalls() const {
        QStringList commands;
        for (const QString& call : AllCalls()) {
            if (call.startsWith("start ") &&
                !call.startsWith("start agents ")) {
                commands.append(call);
            }
        }
        return commands;
    }

    void StartAdapter(const QStringList& args = {}) {
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert(
            "PATH", Dir_->filePath("bin") + u':' + environment.value("PATH"));
        Adapter_ = std::make_unique<QProcess>();
        Adapter_->setProcessEnvironment(environment);
        Adapter_->setProcessChannelMode(QProcess::ForwardedErrorChannel);
        // The poll timer stays out of the way: what the tests see is what the
        // commands caused. A later flag wins.
        QStringList all{"--poll-ms", "600000"};
        all.append(args);
        Adapter_->start(CLAUDE_ADAPTER_PATH, all);
        QVERIFY(Adapter_->waitForStarted(TIMEOUT_MS));
    }

    void Write(const QJsonObject& message) {
        Adapter_->write(QJsonDocument(message).toJson(QJsonDocument::Compact) +
                        '\n');
    }

    void Drain() {
        Buffer_.append(Adapter_->readAllStandardOutput());
        for (qsizetype end = Buffer_.indexOf('\n'); end >= 0;
             end = Buffer_.indexOf('\n')) {
            Received_.append(QJsonDocument::fromJson(Buffer_.first(end)));
            Buffer_.remove(0, end + 1);
        }
    }

    std::optional<QJsonObject> Response(qint64 id) {
        Drain();
        for (const QJsonDocument& message : std::as_const(Received_)) {
            if (message.object().value("id") == QJsonValue(id)) {
                return message.object();
            }
        }
        return std::nullopt;
    }

    QJsonObject Ask(const QString& method, const QJsonObject& params) {
        const qint64 id = NextId_++;
        Write(Request(id, method, params));
        std::optional<QJsonObject> response;
        const bool answered = QTest::qWaitFor(
            [&] {
                Adapter_->waitForReadyRead(10);
                response = Response(id);
                return response.has_value();
            },
            TIMEOUT_MS);
        return answered ? *response : QJsonObject{{"timeout", true}};
    }

    QJsonObject RunCommand(const QString& command, const QJsonValue& args) {
        return Ask("commands.run", Run(command, args));
    }

    QJsonValue ListSessions() {
        const QJsonObject response =
            Ask("agent.listSessions", {{"agent", "claude"}});
        return response.contains("result") ? response.value("result")
                                           : response.value("error");
    }

    // The ids of the removals sent so far, as agent/session.
    QStringList Removals() {
        Drain();
        QStringList removed;
        for (const QJsonDocument& message : std::as_const(Received_)) {
            if (message.object().value("method") ==
                QJsonValue("session.removed")) {
                const QJsonObject params =
                    message.object().value("params").toObject();
                removed.append(params.value("agent").toString() + u'/' +
                               params.value("session").toString());
            }
        }
        return removed;
    }

    // Proves that nothing earlier ran: a command that does run, then the
    // calls hold only it. Its start comes after the starts of earlier runs.
    void VerifyNothingRanBefore() {
        const QJsonObject response =
            RunCommand("claude.stop", QJsonObject{{"session", "last"}});
        QVERIFY2(response.contains("result"),
                 QJsonDocument(response).toJson().constData());
        QCOMPARE(CommandCalls(), QStringList{"start stop last"});
    }

    // The pids the hanging fake wrote: a child of its group, then itself.
    QList<pid_t> HungPids() const {
        QFile file(FakePath("pids"));
        QList<pid_t> pids;
        if (file.open(QIODevice::ReadOnly)) {
            for (const QByteArray& line : file.readAll().split('\n')) {
                if (line.toInt() > 0) {
                    pids.append(line.toInt());
                }
            }
        }
        return pids;
    }

    bool WaitForPids() {
        return QTest::qWaitFor([&] { return HungPids().size() >= 2; },
                               TIMEOUT_MS);
    }

    std::unique_ptr<QTemporaryDir> Dir_;
    std::unique_ptr<QProcess> Adapter_;
    QByteArray Buffer_;
    QList<QJsonDocument> Received_;
    qint64 NextId_ = 1;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
        QVERIFY(QDir(Dir_->path()).mkpath("bin"));
        Put("claude", FAKE_CLAUDE);
        QVERIFY(QFile::setPermissions(
            FakePath("claude"),
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        Put("agents.json", Agents({}));
        Buffer_.clear();
        Received_.clear();
        NextId_ = 1;
    }

    void cleanup() {
        if (Adapter_) {
            // Let it stop its runs; a killed adapter leaves them.
            Adapter_->closeWriteChannel();
            if (!Adapter_->waitForFinished(TIMEOUT_MS)) {
                Adapter_->kill();
                Adapter_->waitForFinished(TIMEOUT_MS);
            }
            Adapter_.reset();
        }
        // Whatever a failed test left of the hanging fake.
        for (pid_t pid : HungPids()) {
            ::kill(pid, SIGKILL);
        }
    }

    void StopRunsClaudeStopWithTheId() {
        StartAdapter();
        const QJsonObject response =
            RunCommand("claude.stop", QJsonObject{{"session", "s1"}});
        QVERIFY2(response.contains("result"),
                 QJsonDocument(response).toJson().constData());
        QVERIFY(response.value("result").isNull());
        QCOMPARE(CommandCalls(), QStringList{"start stop s1"});
    }

    void RemoveRunsOnlyRm() {
        StartAdapter();
        const QJsonObject response =
            RunCommand("claude.remove", QJsonObject{{"session", "s1"}});
        QVERIFY2(response.contains("result"),
                 QJsonDocument(response).toJson().constData());
        QVERIFY(response.value("result").isNull());
        QCOMPARE(CommandCalls(), QStringList{"start rm s1"});
    }

    void RespawnAndRespawnAllArgv() {
        StartAdapter();
        QVERIFY(RunCommand("claude.respawn", QJsonObject{{"session", "s1"}})
                    .contains("result"));
        // A session id is of no use to it, and does no harm.
        QVERIFY(RunCommand("claude.respawnAll", QJsonObject{{"session", "s1"}})
                    .contains("result"));
        QVERIFY(
            RunCommand("claude.respawnAll", QJsonObject{}).contains("result"));
        QVERIFY(RunCommand("claude.respawnAll", QJsonValue::Null)
                    .contains("result"));
        QVERIFY(RunCommand("claude.respawnAll", QJsonValue::Undefined)
                    .contains("result"));
        QCOMPARE(CommandCalls(),
                 (QStringList{"start respawn s1", "start respawn --all",
                              "start respawn --all", "start respawn --all",
                              "start respawn --all"}));
    }

    void ASessionThatLooksLikeAnOptionIsRefused() {
        StartAdapter();
        for (const QString& command :
             {"claude.respawn", "claude.stop", "claude.remove"}) {
            QCOMPARE(ErrorCode(RunCommand(command,
                                          QJsonObject{{"session", "--all"}})),
                     -32602);
        }
        VerifyNothingRanBefore();
    }

    void BadArgsAreInvalidParams_data() {
        QTest::addColumn<QString>("command");
        QTest::addColumn<QJsonValue>("args");
        QTest::newRow("stop, no args")
            << "claude.stop" << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("stop, null args")
            << "claude.stop" << QJsonValue(QJsonValue::Null);
        QTest::newRow("stop, args are a string")
            << "claude.stop" << QJsonValue("s1");
        QTest::newRow("stop, args are an array")
            << "claude.stop" << QJsonValue(QJsonArray{"s1"});
        QTest::newRow("stop, no session")
            << "claude.stop" << QJsonValue(QJsonObject{});
        QTest::newRow("remove, a session that is no string")
            << "claude.remove" << QJsonValue(QJsonObject{{"session", 1}});
        QTest::newRow("remove, an empty session")
            << "claude.remove" << QJsonValue(QJsonObject{{"session", ""}});
        QTest::newRow("respawn, a space")
            << "claude.respawn" << QJsonValue(QJsonObject{{"session", "a b"}});
        QTest::newRow("respawn, a slash")
            << "claude.respawn"
            << QJsonValue(QJsonObject{{"session", "../s1"}});
        QTest::newRow("respawn, a trailing newline")
            << "claude.respawn" << QJsonValue(QJsonObject{{"session", "s1\n"}});
        QTest::newRow("respawn, a dash first")
            << "claude.respawn" << QJsonValue(QJsonObject{{"session", "-s1"}});
        QTest::newRow("respawnAll, args are a string")
            << "claude.respawnAll" << QJsonValue("s1");
        QTest::newRow("respawnAll, args are an array")
            << "claude.respawnAll" << QJsonValue(QJsonArray{});
    }

    void BadArgsAreInvalidParams() {
        QFETCH(QString, command);
        QFETCH(QJsonValue, args);
        StartAdapter();
        QCOMPARE(ErrorCode(RunCommand(command, args)), -32602);
        VerifyNothingRanBefore();
    }

    void AFailureCarriesItsReason() {
        Put("stop.exit", "1");
        Put("stop.stdout", "noise on stdout\n");
        Put("stop.stderr", "warning: x\n\nNo job matching 's1'\nmore\n");
        StartAdapter();
        const QJsonObject response =
            RunCommand("claude.stop", QJsonObject{{"session", "s1"}});
        const QJsonObject error = response.value("error").toObject();
        QCOMPARE(error.value("code").toInt(), -32603);
        QCOMPARE(error.value("message").toString(), "No job matching 's1'");
        QCOMPARE(error.value("data").toObject(),
                 (QJsonObject{
                     {"exitCode", 1},
                     {"stdout", "noise on stdout"},
                     {"stderr", "warning: x\n\nNo job matching 's1'\nmore"}}));
    }

    void AWarningWordInTheReasonIsKept_data() {
        QTest::addColumn<QByteArray>("stderr");
        QTest::addColumn<QString>("reason");
        QTest::newRow("a session named so")
            << QByteArray("No job matching 'warnings-x'\n")
            << "No job matching 'warnings-x'";
        QTest::newRow("a warning before it")
            << QByteArray(
                   "Warning: x\nWARN : y\nNo job matching 'warnings-x'\n")
            << "No job matching 'warnings-x'";
    }

    void AWarningWordInTheReasonIsKept() {
        QFETCH(QByteArray, stderr);
        QFETCH(QString, reason);
        Put("stop.exit", "1");
        Put("stop.stderr", stderr);
        StartAdapter();
        QCOMPARE(
            RunCommand("claude.stop", QJsonObject{{"session", "warnings-x"}})
                .value("error")
                .toObject()
                .value("message")
                .toString(),
            reason);
    }

    void StdoutIsTheReasonWhenStderrHasNone() {
        Put("rm.exit", "1");
        Put("rm.stdout", "refusing: unpushed work\n");
        StartAdapter();
        QCOMPARE(RunCommand("claude.remove", QJsonObject{{"session", "s1"}})
                     .value("error")
                     .toObject()
                     .value("message")
                     .toString(),
                 "refusing: unpushed work");
    }

    void AnEmptyFailureNamesTheExitCode() {
        Put("respawn.exit", "3");
        StartAdapter();
        const QJsonObject error = RunCommand("claude.respawnAll", QJsonObject{})
                                      .value("error")
                                      .toObject();
        QCOMPARE(error.value("code").toInt(), -32603);
        QCOMPARE(error.value("message").toString(),
                 "claude respawn exited with code 3");
        QCOMPARE(error.value("data").toObject().value("exitCode").toInt(), 3);
    }

    void AFailureTooLongForTheErrorIsCut() {
        Put("stop.exit", "1");
        Put("stop.stderr", QByteArray(10000, 'x'));
        StartAdapter();
        const QJsonObject data =
            RunCommand("claude.stop", QJsonObject{{"session", "s1"}})
                .value("error")
                .toObject()
                .value("data")
                .toObject();
        QCOMPARE(data.value("stderr").toString().size(), 4096);
    }

    void AClaudeThatIsMissingFailsTheCommand() {
        // No claude on PATH at all.
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("PATH", Dir_->filePath("empty"));
        Adapter_ = std::make_unique<QProcess>();
        Adapter_->setProcessEnvironment(environment);
        Adapter_->setProcessChannelMode(QProcess::ForwardedErrorChannel);
        Adapter_->start(CLAUDE_ADAPTER_PATH, {"--poll-ms", "600000"});
        QVERIFY(Adapter_->waitForStarted(TIMEOUT_MS));
        const QString message =
            RunCommand("claude.stop", QJsonObject{{"session", "s1"}})
                .value("error")
                .toObject()
                .value("message")
                .toString();
        QVERIFY2(message.startsWith("cannot run claude stop: "),
                 qPrintable(message));
    }

    void AHangingCommandIsKilledWithItsGroup() {
        Put("stop.hang", "");
        StartAdapter({"--command-timeout-ms", "3000"});
        Write(Request(100, "commands.run",
                      Run("claude.stop", QJsonObject{{"session", "s1"}})));
        // The pids come first; the answer waits for the timeout, however
        // long the pids took.
        QVERIFY2(WaitForPids(), "the fake did not write its pids");
        const QList<pid_t> pids = HungPids();
        std::optional<QJsonObject> response;
        QVERIFY(QTest::qWaitFor(
            [&] {
                Adapter_->waitForReadyRead(10);
                response = Response(100);
                return response.has_value();
            },
            2 * TIMEOUT_MS));
        QCOMPARE(response->value("error").toObject(),
                 (QJsonObject{
                     {"code", -32603},
                     {"message", "claude stop did not finish in 3000 ms"}}));
        for (pid_t pid : pids) {
            QTRY_VERIFY_WITH_TIMEOUT(::kill(pid, 0) != 0, TIMEOUT_MS);
        }
    }

    void ARemovalIsListedWithoutWaitingForThePoll() {
        Put("agents.json", Agents({Entry("s1", "idle"), Entry("s2", "idle")}));
        Put("rm.agents", Agents({Entry("s2", "idle")}));
        StartAdapter();
        // The first run has listed both; the poll timer is 10 minutes away.
        QCOMPARE(ListSessions().toArray().size(), 2);
        QVERIFY(RunCommand("claude.remove", QJsonObject{{"session", "s1"}})
                    .contains("result"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                Adapter_->waitForReadyRead(10);
                return Removals() == QStringList{"claude/s1"};
            },
            TIMEOUT_MS));
    }

    void ARunInFlightIsFollowedByAnother() {
        Put("agents.json", Agents({Entry("s1", "idle"), Entry("s2", "idle")}));
        Put("agents.hold", "");
        Put("rm.agents", Agents({Entry("s2", "idle")}));
        StartAdapter();
        // The first run is going, held, and has read both sessions.
        QVERIFY(QTest::qWaitFor(
            [&] { return AllCalls().contains("start agents --json --all"); },
            TIMEOUT_MS));
        // The command ends while that run is still held.
        QVERIFY(RunCommand("claude.remove", QJsonObject{{"session", "s1"}})
                    .contains("result"));
        QVERIFY(QFile::remove(FakePath("agents.hold")));
        // That run lists s1 still; the one after it does not.
        QVERIFY(QTest::qWaitFor(
            [&] {
                Adapter_->waitForReadyRead(10);
                return Removals() == QStringList{"claude/s1"};
            },
            TIMEOUT_MS));
        QCOMPARE(AllCalls().count("start agents --json --all"), 2);
    }

    void ListingIsAnsweredWhileACommandHangs() {
        Put("agents.json", Agents({Entry("s1", "idle")}));
        Put("stop.hang", "");
        StartAdapter();
        Write(Request(100, "commands.run",
                      Run("claude.stop", QJsonObject{{"session", "s1"}})));
        QVERIFY(WaitForPids());
        QCOMPARE(ListSessions().toArray().size(), 1);
        QVERIFY(!Response(100).has_value());
    }

    void AnUnknownCommandIsInvalidParams() {
        StartAdapter();
        QCOMPARE(ErrorCode(RunCommand("claude.nope", QJsonObject{})), -32602);
        QCOMPARE(ErrorCode(RunCommand("stop", QJsonObject{{"session", "s1"}})),
                 -32602);
        QCOMPARE(ErrorCode(Ask("commands.run", QJsonObject{})), -32602);
        // Other methods stay unknown.
        QCOMPARE(ErrorCode(Ask("agent.start", {{"agent", "claude"}})), -32601);
        VerifyNothingRanBefore();
    }

    void StoppingTheAdapterKillsRunningCommands() {
        Put("stop.hang", "");
        StartAdapter();
        Write(Request(100, "commands.run",
                      Run("claude.stop", QJsonObject{{"session", "s1"}})));
        QVERIFY(WaitForPids());
        Adapter_->closeWriteChannel();
        QVERIFY(Adapter_->waitForFinished(TIMEOUT_MS));
        QCOMPARE(Adapter_->exitCode(), 0);
        for (pid_t pid : HungPids()) {
            QTRY_VERIFY_WITH_TIMEOUT(::kill(pid, 0) != 0, TIMEOUT_MS);
        }
        // The request still gets its answer on the way out.
        QCOMPARE(Response(100).value_or(QJsonObject{}).value("error"),
                 QJsonValue(QJsonObject{
                     {"code", -32603},
                     {"message",
                      "the adapter stopped before claude stop finished"}}));
    }

    void TheManifestDeclaresTheCommands() {
        // The built plugin directory under the real roostd: a command that
        // plugin.json misspells has no owner, and the window gets "no plugin
        // has command".
        Put("agents.json", Agents({}));
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        const QString socket = Dir_->filePath("roostd.sock");
        environment.insert("ROOST_SOCKET", socket);
        environment.insert("ROOST_CONFIG", Dir_->filePath("config"));
        environment.insert("ROOST_PLUGINS_PATH", ROOST_PLUGINS_DIR);
        environment.insert(
            "PATH", Dir_->filePath("bin") + u':' + environment.value("PATH"));
        QProcess daemon;
        daemon.setProcessEnvironment(environment);
        daemon.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        daemon.start(ROOSTD_PATH, {});
        QVERIFY(daemon.waitForStarted(TIMEOUT_MS));

        // roostd may still be starting: a client that failed is not reused.
        std::unique_ptr<RpcClient> client;
        const QDeadlineTimer deadline(TIMEOUT_MS);
        while (!client && !deadline.hasExpired()) {
            auto attempt = std::make_unique<RpcClient>();
            const QFuture<rpc::Result> hello = attempt->Connect(socket);
            if (WaitFor(hello) && hello.result()) {
                client = std::move(attempt);
            } else {
                QTest::qWait(20);
            }
        }
        QVERIFY(client);
        const QFuture<rpc::Result> listed = client->Request("plugins.list");
        QVERIFY(WaitFor(listed));
        QVERIFY(listed.result());
        QJsonObject claude;
        for (const QJsonValue& plugin :
             listed.result()->toObject().value("plugins").toArray()) {
            if (plugin.toObject().value("id") == QJsonValue("claude")) {
                claude = plugin.toObject();
            }
        }
        QVERIFY2(!claude.isEmpty(), "roostd did not load the claude plugin");
        QSet<QString> ids;
        for (const QJsonValue& command : claude.value("contributes")
                                             .toObject()
                                             .value("commands")
                                             .toArray()) {
            ids.insert(command.toObject().value("id").toString());
        }
        QCOMPARE(ids, (QSet<QString>{"claude.stop", "claude.remove",
                                     "claude.respawn", "claude.respawnAll"}));

        QCOMPARE(::kill(static_cast<pid_t>(daemon.processId()), SIGTERM), 0);
        QVERIFY(daemon.waitForFinished(TIMEOUT_MS));
    }
};

QTEST_GUILESS_MAIN(ClaudeCommandsTest)
#include "claude_commands_test.moc"
