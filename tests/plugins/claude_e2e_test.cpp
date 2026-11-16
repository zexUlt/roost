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
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <signal.h>

#include <cstdint>
#include <map>
#include <memory>
#include <utility>

#include "claude/backend/agents.hpp"
#include "core/protocol.hpp"
#include "core/rpc_client.hpp"
#include "core/vt_terminal.hpp"

using namespace roost;

namespace {

constexpr int TIMEOUT_MS = 10000;
// Listing the real claude can take a while.
constexpr int REAL_TIMEOUT_MS = 30000;
constexpr const char* ANSWERED_ONCE = "answers da1=1 cpr=1";

// A fake `claude`, steered by the files next to it: agents.json is what
// `agents` prints, and each `attach <id>` adds the id to calls.
constexpr const char* FAKE_CLAUDE = R"sh(#!/bin/sh
dir=$(dirname "$0")
case "$1" in
agents) cat "$dir/agents.json" ;;
attach)
  echo "$2" >> "$dir/calls"
  exec "$dir/fake_tui"
  ;;
*) exit 2 ;;
esac
)sh";

QByteArray ReadFixture() {
    QFile file(QStringLiteral(FIXTURES_DIR "/claude_agents.json"));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

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
    QString CallsPath() const { return Dir_->filePath("bin/calls"); }

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

    // roostd with the Claude plugin, behind the fake claude or not.
    bool Launch(bool fakeClaude) {
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("ROOST_SOCKET", SocketPath());
        environment.insert("ROOST_CONFIG", Dir_->filePath("config"));
        environment.insert("ROOST_PLUGINS_PATH", ROOST_PLUGINS_DIR);
        if (fakeClaude) {
            environment.insert("PATH", Dir_->filePath("bin") + u':' +
                                           environment.value("PATH"));
        }
        Daemon_ = std::make_unique<QProcess>();
        Daemon_->setProcessEnvironment(environment);
        Daemon_->setProcessChannelMode(QProcess::ForwardedErrorChannel);
        Daemon_->start(ROOSTD_PATH, {});
        return Daemon_->waitForStarted(TIMEOUT_MS) && Greets(SocketPath()) &&
               Window_->Connect(SocketPath());
    }

    QStringList Calls() const {
        QFile file(CallsPath());
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QString::fromUtf8(file.readAll())
            .split(u'\n', Qt::SkipEmptyParts);
    }

    std::unique_ptr<QTemporaryDir> Dir_;
    std::unique_ptr<QProcess> Daemon_;
    std::unique_ptr<Window> Window_;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
        Window_ = std::make_unique<Window>();
    }

    void cleanup() {
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

        // The sessions as the adapter maps the fixture, with their agent.
        const auto listing = claude::ParseAgents(ReadFixture());
        QVERIFY(listing);
        QJsonArray expected;
        for (const claude::Session& session : listing->Sessions) {
            QJsonObject json = claude::ToJson(session);
            json.insert("agent", "claude");
            expected.append(json);
        }
        QCOMPARE(expected.size(), 4);
        const rpc::Result listed =
            Window_->Call("sessions.list", QJsonObject{});
        QVERIFY2(listed, qPrintable(Why(listed)));
        QCOMPARE(listed->toObject().value("sessions"), QJsonValue(expected));
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

        QTRY_VERIFY_WITH_TIMEOUT(Window_->Dump(channel).contains(ANSWERED_ONCE),
                                 TIMEOUT_MS);
        QCOMPARE(Calls(), QStringList{id});
        // Each query got exactly one answer: nothing more comes.
        QTest::qWait(500);
        QVERIFY2(Window_->Dump(channel).contains(ANSWERED_ONCE),
                 Window_->Dump(channel).constData());
        QCOMPARE(Calls(), QStringList{id});
    }

    void ListsTheSessionsOfTheRealClaude() {
        if (qgetenv("ROOST_REAL_CLAUDE") != "1") {
            QSKIP("set ROOST_REAL_CLAUDE=1 to list the real claude agents");
        }
        QProcess claude;
        claude.start("claude", {"agents", "--json", "--all"});
        QVERIFY(claude.waitForFinished(20'000));
        QSet<QString> expected;
        for (const QJsonValue& entry :
             QJsonDocument::fromJson(claude.readAllStandardOutput()).array()) {
            if (entry.toObject().value("kind") == QJsonValue("background")) {
                expected.insert(entry.toObject().value("id").toString());
            }
        }

        QVERIFY(Launch(false));
        const rpc::Result listed =
            Window_->Call("sessions.list", QJsonObject{}, REAL_TIMEOUT_MS);
        QVERIFY2(listed, qPrintable(Why(listed)));
        QCOMPARE(listed->toObject().value("unavailable").toArray().size(), 0);
        QSet<QString> found;
        for (const QJsonValue& session :
             listed->toObject().value("sessions").toArray()) {
            found.insert(session.toObject().value("id").toString());
        }
        QCOMPARE(found, expected);
        qInfo("real claude: %d sessions", static_cast<int>(found.size()));
    }
};

QTEST_GUILESS_MAIN(ClaudeE2eTest)
#include "claude_e2e_test.moc"
