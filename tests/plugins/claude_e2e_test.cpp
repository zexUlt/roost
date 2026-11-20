// The Claude plugin end to end: the real roostd binary loads the real Claude
// plugin, with a fake `claude` first on PATH that prints the fixture for
// `claude agents --json --all` and runs fake_tui for `claude attach <id>`. A
// window over the socket lists the sessions and opens one, and its own
// terminal, fed the snapshot and the output, must show that each query of the
// TUI got exactly one answer. With ROOST_REAL_CLAUDE=1 it also lists the
// sessions of the real claude through roostd.

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
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <signal.h>

#include <cstdint>
#include <map>
#include <memory>
#include <utility>

#include "api/api.hpp"
#include "core/protocol.hpp"
#include "core/rpc_client.hpp"
#include "core/vt_terminal.hpp"
#include "helpers/process_lifeline.hpp"

using namespace roost;
using roost::testing::ProcessLifeline;
using roost::testing::RelayStandardError;

namespace {

constexpr int TIMEOUT_MS = 10000;
// Listing the real claude can take a while.
constexpr int REAL_TIMEOUT_MS = 30000;
constexpr int REAL_ATTEMPTS = 3;
// Each query answered once; a digit after the 1 would be more than once.
const QRegularExpression ANSWERED_ONCE(
    QStringLiteral(R"(answers da1=1 cpr=1(?!\d))"));

// The sessions of agents.json as roostd lists them: the states mapped
// (needs_input and blocked to blocked, done with busy to working), a session
// without a name titled by its id.
constexpr const char* EXPECTED_SESSIONS = R"json([
  {"agent": "claude", "id": "bg-0a1b2c3d", "state": "working",
   "title": "Fix the flaky test", "cwd": "/work/alpha",
   "startedAt": 1790150000000},
  {"agent": "claude", "id": "bg-1b2c3d4e", "state": "blocked",
   "title": "Review the PR", "cwd": "/work/beta",
   "startedAt": 1790150100000},
  {"agent": "claude", "id": "bg-2c3d4e5f", "state": "done",
   "title": "bg-2c3d4e5f", "cwd": "/work/gamma",
   "startedAt": 1790150200000},
  {"agent": "claude", "id": "bg-3d4e5f60", "state": "working",
   "title": "Answer a question", "cwd": "/work/delta",
   "startedAt": 1790150300000}
])json";

// A fake `claude`, steered by the files next to it: agents.json is what
// `agents` prints, and each `attach <id>` adds the id to calls and leaves its
// pid. `stop`, `rm` and `respawn` add "<verb> <id>" to commands, fail with the
// exit code in <verb>.exit and the words "No job matching" when that file is
// there, and otherwise end the attach of the id, as the real claude does.
// `rm` also puts rm.agents, when it is there, in place of agents.json.
constexpr const char* FAKE_CLAUDE = R"sh(#!/bin/sh
dir=$(dirname "$0")
case "$1" in
agents)
  echo $PPID > "$dir/adapter.pid"
  cat "$dir/agents.json"
  ;;
attach)
  echo "$2" >> "$dir/calls"
  echo $$ > "$dir/attach-$2.pid"
  exec "$dir/fake_tui"
  ;;
stop|rm|respawn)
  echo "$1 $2" >> "$dir/commands"
  if [ -f "$dir/$1.exit" ]; then
    echo "No job matching '$2'" >&2
    exit $(cat "$dir/$1.exit")
  fi
  if [ -f "$dir/attach-$2.pid" ]; then
    kill $(cat "$dir/attach-$2.pid") 2>/dev/null
    rm -f "$dir/attach-$2.pid"
  fi
  if [ "$1" = rm ] && [ -f "$dir/rm.agents" ]; then
    mv "$dir/rm.agents" "$dir/agents.json"
  fi
  ;;
*) exit 2 ;;
esac
)sh";

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

template <typename TValue>
bool WaitFor(const QFuture<TValue>& future, int timeoutMs) {
    if (future.isFinished()) {
        return true;
    }
    QEventLoop loop;
    QFutureWatcher<TValue> watcher;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop,
                     &QEventLoop::quit);
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    watcher.setFuture(future);
    loop.exec();
    return future.isFinished();
}

// The error to show when a result is missing. QVERIFY2 computes its message
// before the check, so it must not call error() on a value.
QString Why(const rpc::Result& result) {
    return result ? QString() : result.error().Message;
}

// A window: a client with a terminal for each channel, restored from the
// snapshot and fed the output, silent.
class Window {
public:
    Window() {
        QObject::connect(
            &Rpc, &RpcClient::SnapshotReceived, &Rpc,
            [this](std::uint32_t channel, const QByteArray& bytes) {
                auto terminal = VtTerminal::FromSnapshot(bytes);
                if (terminal) {
                    Terminals_.insert_or_assign(channel, std::move(*terminal));
                }
            });
        QObject::connect(
            &Rpc, &RpcClient::OutputReceived, &Rpc,
            [this](std::uint32_t channel, const QByteArray& bytes) {
                const auto it = Terminals_.find(channel);
                if (it != Terminals_.end()) {
                    it->second.Write(bytes);
                }
            });
    }

    bool Connect(const QString& path) {
        const QFuture<rpc::Result> hello = Rpc.Connect(path);
        return WaitFor(hello, TIMEOUT_MS) && hello.result();
    }

    rpc::Result Call(const QString& method, const QJsonValue& params,
                     int timeoutMs = TIMEOUT_MS) {
        const QFuture<rpc::Result> answer = Rpc.Request(method, params);
        if (!WaitFor(answer, timeoutMs)) {
            return std::unexpected(rpc::Error{.Message = "timed out"});
        }
        return answer.result();
    }

    // The window's screen of the channel.
    QByteArray Dump(std::uint32_t channel) const {
        const auto it = Terminals_.find(channel);
        if (it == Terminals_.end()) {
            return "no terminal";
        }
        return it->second.Dump().value_or("dump failed");
    }

    RpcClient Rpc;

private:
    std::map<std::uint32_t, VtTerminal> Terminals_;
};

}  // namespace

class ClaudeE2eTest : public QObject {
    Q_OBJECT

private:
    QString SocketPath() const { return Dir_->filePath("roostd.sock"); }

    // The fake claude with the fixture and fake_tui next to it.
    bool WriteFakeClaude() const {
        const QDir bin(Dir_->filePath("bin"));
        QFile script(bin.filePath("claude"));
        if (!bin.mkpath(".") || !script.open(QIODevice::WriteOnly) ||
            script.write(FAKE_CLAUDE) < 0 ||
            !script.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                                   QFile::ExeOwner)) {
            return false;
        }
        return QFile::copy(QStringLiteral(FIXTURES_DIR "/claude_agents.json"),
                           bin.filePath("agents.json")) &&
               QFile::link(FAKE_TUI_PATH, bin.filePath("fake_tui"));
    }

    // The plugins directory of the test: the built Claude plugin behind a
    // script that runs the adapter with --poll-ms, so that only a poll the
    // adapter takes on a command can show in a short wait.
    bool WriteSlowPollingPlugin(int pollMs) const {
        const QDir plugin(Dir_->filePath("plugins/claude"));
        const QString adapter =
            QStringLiteral(ROOST_PLUGINS_DIR "/claude/backend/claude-adapter");
        QFile built(QStringLiteral(ROOST_PLUGINS_DIR "/claude/plugin.json"));
        if (!plugin.mkpath("backend") || !built.open(QIODevice::ReadOnly)) {
            return false;
        }
        QJsonObject manifest =
            QJsonDocument::fromJson(built.readAll()).object();
        manifest.insert(
            "backend",
            QJsonObject{{"command", QJsonArray{"backend/claude-adapter"}}});
        QFile script(plugin.filePath("backend/claude-adapter"));
        QFile json(plugin.filePath("plugin.json"));
        const QByteArray shell = QStringLiteral(
                                     "#!/bin/sh\nexec '%1' --poll-ms "
                                     "%2 \"$@\"\n")
                                     .arg(adapter)
                                     .arg(pollMs)
                                     .toUtf8();
        return script.open(QIODevice::WriteOnly) &&
               script.write(shell) == shell.size() &&
               script.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                                     QFile::ExeOwner) &&
               json.open(QIODevice::WriteOnly) &&
               json.write(QJsonDocument(manifest).toJson()) > 0;
    }

    // roostd with the Claude plugin, behind the fake claude or not. A
    // pollMs above 0 sets the poll interval of the adapter.
    bool Launch(bool fakeClaude, int pollMs = 0) {
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("ROOST_SOCKET", SocketPath());
        environment.insert("ROOST_CONFIG", Dir_->filePath("config"));
        if (pollMs > 0) {
            // The directory first: of two plugins with an id, the first wins.
            if (!WriteSlowPollingPlugin(pollMs)) {
                return false;
            }
            environment.insert("ROOST_PLUGINS_PATH",
                               Dir_->filePath("plugins") + u':' +
                                   QStringLiteral(ROOST_PLUGINS_DIR));
        } else {
            environment.insert("ROOST_PLUGINS_PATH", ROOST_PLUGINS_DIR);
        }
        if (fakeClaude) {
            environment.insert("PATH", Dir_->filePath("bin") + u':' +
                                           environment.value("PATH"));
        }
        Daemon_ = std::make_unique<QProcess>();
        Daemon_->setProcessEnvironment(environment);
        RelayStandardError(*Daemon_);
        if (!Lifeline_.Attach(*Daemon_)) {
            return false;
        }
        Daemon_->start(ROOSTD_PATH, {});
        return Daemon_->waitForStarted(TIMEOUT_MS) && Greets(SocketPath()) &&
               Window_->Connect(SocketPath());
    }

    QStringList Calls() const { return Lines("bin/calls"); }
    // The commands the fake claude was asked to run: "<verb> <id>".
    QStringList Commands() const { return Lines("bin/commands"); }

    QStringList Lines(const QString& name) const {
        QFile file(Dir_->filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QString::fromUtf8(file.readAll())
            .split(u'\n', Qt::SkipEmptyParts);
    }

    // The ids of the background sessions `claude agents --json --all` prints.
    static QSet<QString> RealSessionIds() {
        QProcess claude;
        claude.start("claude", {"agents", "--json", "--all"});
        QSet<QString> ids;
        if (!claude.waitForFinished(20'000)) {
            return ids;
        }
        for (const QJsonValue& entry :
             QJsonDocument::fromJson(claude.readAllStandardOutput()).array()) {
            if (entry.toObject().value("kind") == QJsonValue("background")) {
                ids.insert(entry.toObject().value("id").toString());
            }
        }
        return ids;
    }

    // The pid of the adapter, as the last run of the fake claude wrote it.
    qint64 AdapterPid() const {
        QFile file(Dir_->filePath("bin/adapter.pid"));
        return file.open(QIODevice::ReadOnly)
                   ? file.readAll().trimmed().toLongLong()
                   : 0;
    }

    // Replaces agents.json whole, so that no run reads half of it.
    bool WriteAgents(const QJsonArray& agents) const {
        QSaveFile file(Dir_->filePath("bin/agents.json"));
        return file.open(QIODevice::WriteOnly) &&
               file.write(
                   QJsonDocument(agents).toJson(QJsonDocument::Compact)) > 0 &&
               file.commit();
    }

    // The window API, connected, with the sessions loaded.
    bool ConnectApi() {
        Api_ = std::make_unique<api::Api>();
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        Api_->Connect(SocketPath());
        // The signal may be said inside Connect: look at the spy before
        // waiting.
        return (!connected.isEmpty() || connected.wait(TIMEOUT_MS)) &&
               QTest::qWaitFor([&] { return Api_->Sessions().IsLoaded(); },
                               TIMEOUT_MS);
    }

    // A command through the window API: Roost.commands.run.
    rpc::Result RunCommand(const QString& command, const QJsonObject& args) {
        const QFuture<rpc::Result> answer = Api_->Commands().Run(command, args);
        if (!WaitFor(answer, TIMEOUT_MS)) {
            return std::unexpected(rpc::Error{.Message = "timed out"});
        }
        return answer.result();
    }

    // Opens the session on the window's connection; the channel, or 0.
    std::uint32_t OpenSession(const QString& id) {
        const rpc::Result opened =
            Window_->Call("session.open", QJsonObject{{"agent", "claude"},
                                                      {"session", id},
                                                      {"columns", 100},
                                                      {"rows", 30}});
        if (!opened) {
            return 0;
        }
        return static_cast<std::uint32_t>(
            opened->toObject().value("channel").toInt());
    }

    // The fake claude has written the pid of the attach of the session, so
    // that stop, rm and respawn find a process to end.
    bool AttachStarted(const QString& id) const {
        QFile file(Dir_->filePath("bin/attach-" + id + ".pid"));
        return file.open(QIODevice::ReadOnly) && file.readAll().endsWith('\n');
    }

    // The window has been told that the session of the channel ended.
    static bool HasClosed(const QSignalSpy& notified, std::uint32_t channel) {
        for (const QList<QVariant>& arguments : notified) {
            if (arguments.at(0).toString() == "session.closed" &&
                arguments.at(1)
                        .value<QJsonValue>()
                        .toObject()
                        .value("channel")
                        .toInt() == static_cast<int>(channel)) {
                return true;
            }
        }
        return false;
    }

    // Replaces the file of the fake claude whole.
    bool Put(const QString& name, const QByteArray& content) const {
        QSaveFile file(Dir_->filePath("bin/" + name));
        return file.open(QIODevice::WriteOnly) &&
               file.write(content) == content.size() && file.commit();
    }

    std::unique_ptr<QTemporaryDir> Dir_;
    ProcessLifeline Lifeline_;
    std::unique_ptr<QProcess> Daemon_;
    // An adapter the test has stopped; it must not outlive the test.
    qint64 StoppedAdapter_ = 0;
    std::unique_ptr<api::Api> Api_;
    std::unique_ptr<Window> Window_;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
        Window_ = std::make_unique<Window>();
    }

    void cleanup() {
        if (StoppedAdapter_ > 0) {
            // A stopped process takes no SIGTERM.
            ::kill(static_cast<pid_t>(StoppedAdapter_), SIGCONT);
            ::kill(static_cast<pid_t>(StoppedAdapter_), SIGKILL);
            StoppedAdapter_ = 0;
        }
        Api_.reset();
        Window_.reset();
        if (Daemon_ && Daemon_->state() != QProcess::NotRunning) {
            QCOMPARE(::kill(static_cast<pid_t>(Daemon_->processId()), SIGTERM),
                     0);
            QVERIFY(Daemon_->waitForFinished(TIMEOUT_MS));
            QCOMPARE(Daemon_->exitStatus(), QProcess::NormalExit);
            QCOMPARE(Daemon_->exitCode(), 0);
        }
        Daemon_.reset();
    }

    void ListsAndOpensTheSessionsOfTheClaudePlugin() {
        QVERIFY(WriteFakeClaude());
        QVERIFY(Launch(true));

        const QJsonArray expected =
            QJsonDocument::fromJson(EXPECTED_SESSIONS).array();
        QCOMPARE(expected.size(), 4);
        const rpc::Result listed =
            Window_->Call("sessions.list", QJsonObject{});
        QVERIFY2(listed, qPrintable(Why(listed)));
        QCOMPARE(listed->toObject().value("sessions").toArray(), expected);
        QCOMPARE(listed->toObject().value("unavailable"),
                 QJsonValue(QJsonArray()));

        const QString id = "bg-1b2c3d4e";
        const rpc::Result opened =
            Window_->Call("session.open", QJsonObject{{"agent", "claude"},
                                                      {"session", id},
                                                      {"columns", 100},
                                                      {"rows", 30}});
        QVERIFY2(opened, qPrintable(Why(opened)));
        const auto channel = static_cast<std::uint32_t>(
            opened->toObject().value("channel").toInt());
        QVERIFY(channel > 0);

        QTRY_VERIFY_WITH_TIMEOUT(
            QString::fromUtf8(Window_->Dump(channel)).contains(ANSWERED_ONCE),
            TIMEOUT_MS);
        QCOMPARE(Calls(), QStringList{id});
        // Each query got exactly one answer: nothing more comes.
        QTest::qWait(500);
        QVERIFY2(
            QString::fromUtf8(Window_->Dump(channel)).contains(ANSWERED_ONCE),
            Window_->Dump(channel).constData());
        QCOMPARE(Calls(), QStringList{id});
    }

    void ARemovedClaudeSessionReachesTheWindow() {
        QVERIFY(WriteFakeClaude());
        QVERIFY(Launch(true));
        // The first list is in, so the adapter has its first run behind it.
        const rpc::Result listed =
            Window_->Call("sessions.list", QJsonObject{});
        QVERIFY2(listed, qPrintable(Why(listed)));
        QCOMPARE(listed->toObject().value("sessions").toArray().size(), 4);
        const rpc::Result subscribed = Window_->Call(
            "bus.subscribe", QJsonObject{{"pattern", "session.removed"}});
        QVERIFY2(subscribed, qPrintable(Why(subscribed)));
        QSignalSpy notified(&Window_->Rpc, &RpcClient::NotificationReceived);

        // claude no longer lists the session; the file is replaced whole, so
        // that no run reads half of it.
        const QString gone = "bg-2c3d4e5f";
        QJsonArray left;
        QFile source(Dir_->filePath("bin/agents.json"));
        QVERIFY(source.open(QIODevice::ReadOnly));
        for (const QJsonValue& entry :
             QJsonDocument::fromJson(source.readAll()).array()) {
            if (entry.toObject().value("id").toString() != gone) {
                left.append(entry);
            }
        }
        QSaveFile agents(Dir_->filePath("bin/agents.json"));
        QVERIFY(agents.open(QIODevice::WriteOnly));
        agents.write(QJsonDocument(left).toJson(QJsonDocument::Compact));
        QVERIFY(agents.commit());

        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (const QList<QVariant>& arguments : notified) {
                    const QJsonObject event =
                        arguments.at(1).value<QJsonValue>().toObject();
                    if (arguments.at(0).toString() == "bus.event" &&
                        event.value("name").toString() == "session.removed" &&
                        event.value("payload") ==
                            QJsonValue(QJsonObject{{"agent", "claude"},
                                                   {"id", gone}})) {
                        return true;
                    }
                }
                return false;
            }(),
            TIMEOUT_MS);
        const rpc::Result after = Window_->Call("sessions.list", QJsonObject{});
        QVERIFY2(after, qPrintable(Why(after)));
        QCOMPARE(after->toObject().value("sessions").toArray().size(), 3);
    }

    // What the adapter did while it was down is told by no event: the window
    // lists the sessions again when roostd announces the restart.
    void ASessionRemovedWhileTheAdapterIsDownLeavesTheWindow() {
        QVERIFY(WriteFakeClaude());
        QVERIFY(Launch(true));
        Api_ = std::make_unique<api::Api>();
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        Api_->Connect(SocketPath());
        QVERIFY(!connected.isEmpty() || connected.wait(TIMEOUT_MS));
        QTRY_VERIFY_WITH_TIMEOUT(Api_->Sessions().IsLoaded(), TIMEOUT_MS);
        QCOMPARE(Api_->Sessions().Rows().size(), 4);
        const qint64 adapter = AdapterPid();
        QVERIFY(adapter > 1);

        // The adapter is stopped before claude changes, so it sees none of it.
        StoppedAdapter_ = adapter;
        QCOMPARE(::kill(static_cast<pid_t>(adapter), SIGSTOP), 0);
        const QString gone = "bg-2c3d4e5f";
        const QString changed = "bg-1b2c3d4e";
        QJsonArray left;
        QFile source(Dir_->filePath("bin/agents.json"));
        QVERIFY(source.open(QIODevice::ReadOnly));
        for (const QJsonValue& entry :
             QJsonDocument::fromJson(source.readAll()).array()) {
            QJsonObject agent = entry.toObject();
            if (agent.value("id").toString() == gone) {
                continue;
            }
            if (agent.value("id").toString() == changed) {
                agent.insert("state", "done");
            }
            left.append(agent);
        }
        source.close();
        QVERIFY(WriteAgents(left));
        // roostd starts the adapter again.
        QCOMPARE(::kill(static_cast<pid_t>(adapter), SIGKILL), 0);
        StoppedAdapter_ = 0;

        QTRY_COMPARE_WITH_TIMEOUT(Api_->Sessions().IndexOf("claude", gone), -1,
                                  TIMEOUT_MS);
        QCOMPARE(Api_->Sessions().Rows().size(), 3);
        const int index = Api_->Sessions().IndexOf("claude", changed);
        QVERIFY(index >= 0);
        QCOMPARE(Api_->Sessions().Rows()[index].value("state"),
                 QJsonValue("done"));
    }

    // Window -> Api -> roostd -> adapter -> claude rm. The adapter lists at
    // once after the command; the poll, here a minute, is too late.
    void RemoveThroughTheWindow() {
        QVERIFY(WriteFakeClaude());
        // A poll every minute: the list can change at once only through the
        // poll the adapter takes after the command.
        QVERIFY(Launch(true, 60'000));
        QVERIFY(ConnectApi());
        const QString gone = "bg-2c3d4e5f";
        QCOMPARE(Api_->Sessions().Rows().size(), 4);
        const std::uint32_t channel = OpenSession(gone);
        QVERIFY(channel > 0);
        QTRY_VERIFY_WITH_TIMEOUT(AttachStarted(gone), TIMEOUT_MS);
        QSignalSpy notified(&Window_->Rpc, &RpcClient::NotificationReceived);

        // What claude lists after rm: agents.json without the session.
        QJsonArray left;
        QFile source(Dir_->filePath("bin/agents.json"));
        QVERIFY(source.open(QIODevice::ReadOnly));
        for (const QJsonValue& entry :
             QJsonDocument::fromJson(source.readAll()).array()) {
            if (entry.toObject().value("id").toString() != gone) {
                left.append(entry);
            }
        }
        QVERIFY(Put("rm.agents",
                    QJsonDocument(left).toJson(QJsonDocument::Compact)));

        const rpc::Result removed =
            RunCommand("claude.remove", QJsonObject{{"session", gone}});
        QVERIFY2(removed, qPrintable(Why(removed)));
        QVERIFY(removed->isNull());
        QCOMPARE(Commands(), QStringList{"rm " + gone});
        QTRY_VERIFY_WITH_TIMEOUT(HasClosed(notified, channel), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Api_->Sessions().IndexOf("claude", gone), -1,
                                  5000);
        QCOMPARE(Api_->Sessions().Rows().size(), 3);
    }

    // roostd and the window API hand the error of the backend on whole.
    void AFailedRemoveReachesTheWindowWithItsData() {
        QVERIFY(WriteFakeClaude());
        QVERIFY(Launch(true));
        QVERIFY(ConnectApi());
        QVERIFY(Put("rm.exit", "1"));

        const rpc::Result removed = RunCommand(
            "claude.remove", QJsonObject{{"session", "bg-2c3d4e5f"}});
        QVERIFY(!removed);
        QCOMPARE(removed.error().Code, -32603);
        QCOMPARE(removed.error().Message, "No job matching 'bg-2c3d4e5f'");
        const QJsonObject data = removed.error().Data.toObject();
        QCOMPARE(data.value("exitCode").toInt(), 1);
        QVERIFY(data.value("stderr").toString().contains("No job matching"));
        QCOMPARE(Api_->Sessions().Rows().size(), 4);
    }

    // The recipe of the window after claude.respawn: the old attach ends, and
    // opening the session again starts a new one.
    void RespawnThenReopenGetsANewAttach() {
        QVERIFY(WriteFakeClaude());
        QVERIFY(Launch(true));
        QVERIFY(ConnectApi());
        const QString id = "bg-1b2c3d4e";
        const std::uint32_t first = OpenSession(id);
        QVERIFY(first > 0);
        QTRY_COMPARE_WITH_TIMEOUT(Calls(), QStringList{id}, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(AttachStarted(id), TIMEOUT_MS);
        QSignalSpy notified(&Window_->Rpc, &RpcClient::NotificationReceived);

        const rpc::Result respawned =
            RunCommand("claude.respawn", QJsonObject{{"session", id}});
        QVERIFY2(respawned, qPrintable(Why(respawned)));
        QCOMPARE(Commands(), QStringList{"respawn " + id});
        QTRY_VERIFY_WITH_TIMEOUT(HasClosed(notified, first), TIMEOUT_MS);

        const std::uint32_t second = OpenSession(id);
        QVERIFY(second > 0);
        QVERIFY(second != first);
        QTRY_COMPARE_WITH_TIMEOUT(Calls(), (QStringList{id, id}), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            QString::fromUtf8(Window_->Dump(second)).contains(ANSWERED_ONCE),
            TIMEOUT_MS);
    }

    void ListsTheSessionsOfTheRealClaude() {
        if (qgetenv("ROOST_REAL_CLAUDE") != "1") {
            QSKIP("set ROOST_REAL_CLAUDE=1 to list the real claude agents");
        }
        if (QStandardPaths::findExecutable("claude").isEmpty()) {
            QSKIP("claude is not on PATH");
        }
        QVERIFY(Launch(false));
        // The two listings are taken at different moments: retake both when
        // the sessions changed in between.
        QSet<QString> expected;
        QSet<QString> found;
        for (int attempt = 0; attempt < REAL_ATTEMPTS; ++attempt) {
            expected = RealSessionIds();
            if (attempt == 0 && expected.isEmpty()) {
                QSKIP("the real claude has no background sessions");
            }
            const rpc::Result listed =
                Window_->Call("sessions.list", QJsonObject{}, REAL_TIMEOUT_MS);
            QVERIFY2(listed, qPrintable(Why(listed)));
            QCOMPARE(listed->toObject().value("unavailable").toArray().size(),
                     0);
            found.clear();
            for (const QJsonValue& session :
                 listed->toObject().value("sessions").toArray()) {
                found.insert(session.toObject().value("id").toString());
            }
            if (found == expected) {
                break;
            }
        }
        QCOMPARE(found, expected);
        qInfo("real claude: %d sessions", static_cast<int>(found.size()));
    }
};

QTEST_GUILESS_MAIN(ClaudeE2eTest)
#include "claude_e2e_test.moc"
