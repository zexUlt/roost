// Session channels (design.md §4) end to end: a real RpcClient opens channels
// over the socket of a real Server on sessions of fake_tui, which the warm
// pool runs in a PTY, and the attach commands come from an adapter served by
// a plugin backend.

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "core/protocol.hpp"
#include "core/rpc_client.hpp"
#include "core/vt_terminal.hpp"
#include "roostd/backend.hpp"
#include "roostd/manifest.hpp"
#include "roostd/plugins.hpp"
#include "roostd/server.hpp"
#include "roostd/session_channels.hpp"

using namespace roost;
using namespace std::chrono_literals;

namespace {

constexpr int TIMEOUT_MS = 5000;
// How long a test waits to see that something does not happen.
constexpr int NOTHING_HAPPENS_MS = 300;
// How long the adapter sits on the attach command of session "slow".
constexpr int SLOW_ATTACH_MS = 1000;
// Prints 3000 lines in six bursts, so that a second window can open the
// session while the output goes on, then "done", and waits.
constexpr const char* STREAM_SCRIPT =
    "i=0; while [ $i -lt 3000 ]; do echo line$i; i=$((i+1)); "
    "if [ $((i % 500)) -eq 0 ]; then sleep 0.05; fi; done; "
    "echo done; exec sleep 60";
constexpr int INVALID_PARAMS = static_cast<int>(rpc::EErrorCode::InvalidParams);
constexpr int INTERNAL_ERROR = static_cast<int>(rpc::EErrorCode::InternalError);

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

// The error to show when a result is missing. QVERIFY2 computes its message
// before the check, so it must not call error() on a value.
QString Why(const rpc::Result& result) {
    return result ? QString() : result.error().Message;
}

template <typename TValue>
QString WhyNot(const std::expected<TValue, QString>& expected) {
    return expected ? QString() : expected.error();
}

QJsonObject Result(const QJsonValue& value) { return {{"result", value}}; }

WarmSessionKey Key(const QString& session) {
    return WarmSessionKey{.Agent = "fake", .Session = session};
}

// The screen of the terminal a snapshot of the daemon's session makes.
QByteArray DaemonDump(SessionChannels& channels, const QString& session) {
    const WarmSession* warm = channels.Pool().Find(Key(session));
    if (warm == nullptr) {
        return "no warm session";
    }
    const auto snapshot = warm->Snapshot();
    if (!snapshot) {
        return "snapshot: " + snapshot.error().toUtf8();
    }
    const auto terminal = VtTerminal::FromSnapshot(*snapshot);
    if (!terminal) {
        return "restore: " + terminal.error().toUtf8();
    }
    return terminal->Dump().value_or("dump failed");
}

// A window: a client with a terminal for each channel, restored from the
// snapshot and fed the output, silent. Log has everything that arrived, in
// the order it did, the answers to session.open included.
class Window {
public:
    Window() {
        QObject::connect(
            &Rpc, &RpcClient::SnapshotReceived, &Rpc,
            [this](std::uint32_t channel, const QByteArray& bytes) {
                Log << QString("snapshot %1").arg(channel);
                auto terminal = VtTerminal::FromSnapshot(bytes);
                if (!terminal) {
                    Log << "bad snapshot: " + terminal.error();
                    return;
                }
                Terminals_.insert_or_assign(channel, std::move(*terminal));
            });
        QObject::connect(
            &Rpc, &RpcClient::OutputReceived, &Rpc,
            [this](std::uint32_t channel, const QByteArray& bytes) {
                Log << QString("output %1").arg(channel);
                const auto it = Terminals_.find(channel);
                if (it == Terminals_.end()) {
                    Log << QString("output %1 without a snapshot").arg(channel);
                    return;
                }
                it->second.Write(bytes);
            });
        QObject::connect(
            &Rpc, &RpcClient::NotificationReceived, &Rpc,
            [this](const QString& method, const QJsonValue& params) {
                const QJsonObject object = params.toObject();
                Log << QString("%1 %2 %3")
                           .arg(method)
                           .arg(object.value("channel").toInt())
                           .arg(object.value("reason").toString());
                if (method == "session.closed") {
                    Closed.append(object);
                }
            });
    }

    bool Connect(const QString& path) {
        const QFuture<rpc::Result> hello = Rpc.Connect(path);
        return WaitFor(hello) && hello.result();
    }

    rpc::Result Call(const QString& method,
                     const QJsonValue& params = QJsonObject{}) {
        const QFuture<rpc::Result> answer = Rpc.Request(method, params);
        if (!WaitFor(answer)) {
            return std::unexpected(rpc::Error{.Message = "timed out"});
        }
        return answer.result();
    }

    // session.open, with its answer in Log; the channel it opened, or 0.
    std::uint32_t Open(const QString& session,
                       const QJsonObject& size = QJsonObject{}) {
        QJsonObject params{{"agent", "fake"}, {"session", session}};
        for (auto it = size.begin(); it != size.end(); ++it) {
            params.insert(it.key(), it.value());
        }
        const rpc::Result opened = OpenWith(params);
        LastOpen = opened;
        return opened ? static_cast<std::uint32_t>(
                            opened->toObject().value("channel").toInt())
                      : 0;
    }

    // No continuation context: the answer is logged from inside the
    // dispatch, in the order of the frames.
    rpc::Result OpenWith(const QJsonValue& params) {
        QFuture<rpc::Result> answer =
            Rpc.Request("session.open", params)
                .then(QtFuture::Launch::Sync, [this](const rpc::Result& r) {
                    Log << (r ? QString("answer %1")
                                    .arg(r->toObject().value("channel").toInt())
                              : QString("error %1").arg(r.error().Code));
                    return r;
                });
        if (!WaitFor(answer)) {
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

    bool WaitDump(std::uint32_t channel, const QByteArray& text) const {
        return QTest::qWaitFor([&] { return Dump(channel).contains(text); },
                               TIMEOUT_MS);
    }

    bool Resized(std::uint32_t channel, quint16 columns, quint16 rows) {
        const auto it = Terminals_.find(channel);
        return it != Terminals_.end() && it->second.Resize(columns, rows);
    }

    RpcClient Rpc;
    QStringList Log;
    QList<QJsonObject> Closed;
    rpc::Result LastOpen = QJsonValue();

private:
    std::map<std::uint32_t, VtTerminal> Terminals_;
};

}  // namespace

class SessionChannelsTest : public QObject {
    Q_OBJECT

private:
    QString PluginsPath() const { return Dir_->filePath("plugins"); }
    QString SocketPath() const { return Dir_->filePath("roostd.sock"); }

    // A plugin that serves the agents "fake" and "other"; sessions s1, s2 and
    // s3 of "fake" and s1 of "other" attach to fake_tui, stream to a shell
    // loop, and null, broken and missing do not attach.
    bool WritePlugin() {
        const QDir directory(PluginsPath() + "/one");
        if (!directory.mkpath(".") ||
            !QFile::link(FIXTURE_BACKEND_PATH, directory.filePath("backend"))) {
            return false;
        }
        const QJsonObject manifest{
            {"id", "one"},
            {"version", "1"},
            {"api", PLUGIN_API},
            {"backend", QJsonObject{{"command", QJsonArray{"backend"}}}},
            {"contributes",
             QJsonObject{
                 {"agents",
                  QJsonArray{QJsonObject{{"id", "fake"},
                                         {"capabilities",
                                          QJsonArray{"nativeTuiAttach"}}},
                             QJsonObject{{"id", "other"},
                                         {"capabilities",
                                          QJsonArray{"nativeTuiAttach"}}}}}}},
        };
        QFile file(directory.filePath("plugin.json"));
        if (!file.open(QIODevice::WriteOnly)) {
            return false;
        }
        file.write(QJsonDocument(manifest).toJson());
        file.close();

        const QJsonObject fakeTui =
            Result(QJsonObject{{"program", FAKE_TUI_PATH}});
        const QJsonObject answers{
            {"agent.attachCommand",
             QJsonObject{
                 {"fake/s1", fakeTui},
                 {"fake/s2", fakeTui},
                 {"fake/s3", fakeTui},
                 {"fake/slow",
                  QJsonObject{
                      {"result", QJsonObject{{"program", FAKE_TUI_PATH}}},
                      {"delayMs", SLOW_ATTACH_MS}}},
                 {"other/s1", fakeTui},
                 {"fake/stream",
                  Result(
                      QJsonObject{{"program", "/bin/sh"},
                                  {"args", QJsonArray{"-c", STREAM_SCRIPT}}})},
                 {"fake/null", Result(QJsonValue())},
                 {"fake/broken",
                  QJsonObject{{"error", QJsonObject{{"code", 5},
                                                    {"message", "no way"}}}}},
                 {"fake/missing",
                  Result(QJsonObject{{"program", "/nonexistent/roost-agent"}})},
             }}};
        QFile agents(directory.filePath("agent.json"));
        if (!agents.open(QIODevice::WriteOnly)) {
            return false;
        }
        agents.write(QJsonDocument(answers).toJson());
        return true;
    }

    bool Start(WarmPoolOptions options = {}) {
        if (!WritePlugin()) {
            return false;
        }
        Server_ = std::make_unique<Server>();
        if (!Server_->Listen(SocketPath())) {
            return false;
        }
        Host_ = std::make_unique<PluginHost>(
            *Server_, BackendOptions{.FirstRestartDelay = 10ms,
                                     .MaxRestartDelay = 10ms,
                                     .MaxRestarts = 5,
                                     .StableRun = 60s,
                                     .StopTimeout = 1s});
        Host_->Load({PluginsPath()});
        Channels_ =
            std::make_unique<SessionChannels>(*Server_, *Host_, options);
        return true;
    }

    // A greeted window.
    Window* Connect() {
        auto window = std::make_unique<Window>();
        if (!window->Connect(SocketPath())) {
            return nullptr;
        }
        Windows_.push_back(std::move(window));
        return Windows_.back().get();
    }

    // sessions.warm as "agent/id", most recently viewed first.
    static QStringList WarmList(Window& window) {
        QStringList keys;
        const rpc::Result warm = window.Call("sessions.warm");
        if (!warm) {
            return keys;
        }
        for (const QJsonValue session :
             warm->toObject().value("sessions").toArray()) {
            keys << session.toObject().value("agent").toString() + u'/' +
                        session.toObject().value("id").toString();
        }
        return keys;
    }

    std::unique_ptr<QTemporaryDir> Dir_;
    std::unique_ptr<Server> Server_;
    std::unique_ptr<PluginHost> Host_;
    std::unique_ptr<SessionChannels> Channels_;
    std::vector<std::unique_ptr<Window>> Windows_;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
    }

    void cleanup() {
        Windows_.clear();
        Channels_.reset();
        Host_.reset();
        Server_.reset();
    }

    void TheAnswerComesBeforeTheSnapshotAndTheSnapshotBeforeTheOutput() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        const std::uint32_t channel = window->Open("s1");
        QVERIFY2(channel == 1, qPrintable(Why(window->LastOpen)));
        QVERIFY(window->WaitDump(channel, "answers da1=1 cpr=1"));
        QVERIFY2(window->Log.size() >= 3, qPrintable(window->Log.join('\n')));
        QCOMPARE(window->Log.at(0), QString("answer 1"));
        QCOMPARE(window->Log.at(1), QString("snapshot 1"));
        for (qsizetype i = 2; i < window->Log.size(); ++i) {
            QCOMPARE(window->Log.at(i), QString("output 1"));
        }
        QCOMPARE(Channels_->ChannelCount(), 1);
        QCOMPARE(Channels_->Pool().Count(), 1);
    }

    void AWarmSessionAnswersBeforeItsSnapshotToo() {
        QVERIFY(Start());
        Window* first = Connect();
        Window* second = Connect();
        QVERIFY(first && second);
        const std::uint32_t channel = first->Open("s1");
        QVERIFY(channel);
        QVERIFY(first->WaitDump(channel, "answers da1=1 cpr=1"));
        QVERIFY(second->Open("s1"));
        QVERIFY(second->WaitDump(1, "answers da1=1 cpr=1"));
        QCOMPARE(second->Log.at(0), QString("answer 1"));
        QCOMPARE(second->Log.at(1), QString("snapshot 1"));
    }

    void TheWindowFollowsTheSessionThroughTheChannel() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        const std::uint32_t channel = window->Open("s1");
        QVERIFY2(channel, qPrintable(Why(window->LastOpen)));
        // The key must come in a read of its own, after the first answers.
        QVERIFY(window->WaitDump(channel, "answers da1=1 cpr=1"));
        window->Rpc.SendInput(channel, "q");
        QVERIFY2(window->WaitDump(channel, "answers da1=2 cpr=2"),
                 window->Dump(channel).constData());
        window->Rpc.SendInput(channel, "z");
        QVERIFY2(window->WaitDump(channel, "input z total=1"),
                 window->Dump(channel).constData());
        QCOMPARE(window->Dump(channel), DaemonDump(*Channels_, "s1"));
    }

    void OpeningAWarmSessionAgainGivesAnotherChannelOfTheSameProgram() {
        QVERIFY(Start());
        Window* first = Connect();
        Window* second = Connect();
        QVERIFY(first && second);
        const std::uint32_t one = first->Open("s1");
        QVERIFY(one);
        QVERIFY(first->WaitDump(one, "answers da1=1 cpr=1"));
        const qint64 pid = Channels_->Pool().Find(Key("s1"))->ProcessId();

        // On the same connection the number goes on.
        QCOMPARE(first->Open("s1"), 2U);
        const std::uint32_t other = second->Open("s1");
        QVERIFY2(other, qPrintable(Why(second->LastOpen)));
        QCOMPARE(Channels_->ChannelCount(), 3);
        QCOMPARE(Channels_->Pool().Count(), 1);
        QCOMPARE(Channels_->Pool().Find(Key("s1"))->ProcessId(), pid);
        QVERIFY(second->WaitDump(other, "answers da1=1 cpr=1"));
        QCOMPARE(second->Dump(other), DaemonDump(*Channels_, "s1"));
        QCOMPARE(first->Dump(2), DaemonDump(*Channels_, "s1"));

        // All of its channels see what the program does.
        second->Rpc.SendInput(other, "q");
        QVERIFY(first->WaitDump(one, "answers da1=2 cpr=2"));
        QVERIFY(first->WaitDump(2, "answers da1=2 cpr=2"));
        QVERIFY(second->WaitDump(other, "answers da1=2 cpr=2"));
    }

    void TheSessionIsOpenedAtTheSizeAsked() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        const std::uint32_t channel =
            window->Open("s1", {{"columns", 100}, {"rows", 30}});
        QVERIFY2(channel, qPrintable(Why(window->LastOpen)));
        QCOMPARE(Channels_->Pool().Find(Key("s1"))->Size(),
                 (PtySize{.Columns = 100, .Rows = 30}));
        QVERIFY(window->WaitDump(channel, "size 100x30"));

        const std::uint32_t usual = window->Open("s2");
        QVERIFY(usual);
        QCOMPARE(Channels_->Pool().Find(Key("s2"))->Size(), PtySize{});

        // A warm session is resized to a size that is asked, and left alone
        // when none is.
        QVERIFY(window->Open("s1"));
        QCOMPARE(Channels_->Pool().Find(Key("s1"))->Size(),
                 (PtySize{.Columns = 100, .Rows = 30}));
        QVERIFY(window->Open("s1", {{"columns", 90}, {"rows", 20}}));
        QCOMPARE(Channels_->Pool().Find(Key("s1"))->Size(),
                 (PtySize{.Columns = 90, .Rows = 20}));
    }

    void ResizeReachesTheProgram() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        const std::uint32_t channel = window->Open("s1");
        QVERIFY(channel);
        QVERIFY(window->WaitDump(channel, "size 80x24"));

        const rpc::Result resized =
            window->Call("session.resize",
                         QJsonObject{{"channel", static_cast<int>(channel)},
                                     {"columns", 100},
                                     {"rows", 30}});
        QVERIFY2(resized, qPrintable(Why(resized)));
        QCOMPARE(*resized, QJsonValue());
        QVERIFY(window->Resized(channel, 100, 30));
        QCOMPARE(Channels_->Pool().Find(Key("s1"))->Size(),
                 (PtySize{.Columns = 100, .Rows = 30}));
        QVERIFY2(window->WaitDump(channel, "size 100x30"),
                 window->Dump(channel).constData());
    }

    void ABadResizeIsInvalidParams_data() {
        QTest::addColumn<QJsonObject>("params");
        QTest::newRow("unknown channel")
            << QJsonObject{{"channel", 99}, {"columns", 90}, {"rows", 20}};
        QTest::newRow("channel zero")
            << QJsonObject{{"channel", 0}, {"columns", 90}, {"rows", 20}};
        QTest::newRow("no channel")
            << QJsonObject{{"columns", 90}, {"rows", 20}};
        QTest::newRow("channel as string")
            << QJsonObject{{"channel", "1"}, {"columns", 90}, {"rows", 20}};
        QTest::newRow("no columns")
            << QJsonObject{{"channel", 1}, {"rows", 20}};
        QTest::newRow("no rows")
            << QJsonObject{{"channel", 1}, {"columns", 90}};
        QTest::newRow("zero columns")
            << QJsonObject{{"channel", 1}, {"columns", 0}, {"rows", 20}};
        QTest::newRow("zero rows")
            << QJsonObject{{"channel", 1}, {"columns", 90}, {"rows", 0}};
        QTest::newRow("too many columns")
            << QJsonObject{{"channel", 1}, {"columns", 2001}, {"rows", 20}};
        QTest::newRow("too many rows")
            << QJsonObject{{"channel", 1}, {"columns", 90}, {"rows", 2001}};
        QTest::newRow("fractional rows")
            << QJsonObject{{"channel", 1}, {"columns", 90}, {"rows", 20.5}};
        QTest::newRow("columns as string")
            << QJsonObject{{"channel", 1}, {"columns", "90"}, {"rows", 20}};
    }

    void ABadResizeIsInvalidParams() {
        QFETCH(QJsonObject, params);
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        QCOMPARE(window->Open("s1"), 1U);
        const rpc::Result resized = window->Call("session.resize", params);
        QVERIFY(!resized);
        QCOMPARE(resized.error().Code, INVALID_PARAMS);
        QCOMPARE(Channels_->Pool().Find(Key("s1"))->Size(), PtySize{});
    }

    void ClosingAChannelLeavesTheSessionWarm() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        const std::uint32_t channel = window->Open("s1");
        QVERIFY(channel);
        QVERIFY(window->WaitDump(channel, "answers da1=1 cpr=1"));

        const rpc::Result closed =
            window->Call("session.close",
                         QJsonObject{{"channel", static_cast<int>(channel)}});
        QVERIFY2(closed, qPrintable(Why(closed)));
        QCOMPARE(*closed, QJsonValue());
        QCOMPARE(Channels_->ChannelCount(), 0);

        // x would end the program. Input after the channel is closed does not
        // reach it: a later key, from a new channel, finds the program alive.
        window->Rpc.SendInput(channel, "x");
        const std::uint32_t again = window->Open("s1");
        QCOMPARE(again, 2U);
        window->Rpc.SendInput(again, "q");
        QVERIFY2(window->WaitDump(again, "answers da1=2 cpr=2"),
                 window->Dump(again).constData());
        QVERIFY(!window->Dump(again).contains("bye"));
        QVERIFY(!Channels_->Pool().Find(Key("s1"))->IsClosed());
        QCOMPARE(Channels_->Pool().Count(), 1);
        for (const QString& entry : std::as_const(window->Log)) {
            QVERIFY2(!entry.startsWith("session.closed"), qPrintable(entry));
        }
        // The closed channel gets nothing more.
        const qsizetype outputs = window->Log.count("output 1");
        QTest::qWait(50);
        QCOMPARE(window->Log.count("output 1"), outputs);
    }

    void ClosingAChannelThatIsNotThereIsInvalidParams() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        const std::uint32_t channel = window->Open("s1");
        QVERIFY(channel);
        const rpc::Result unknown =
            window->Call("session.close", QJsonObject{{"channel", 99}});
        QVERIFY(!unknown);
        QCOMPARE(unknown.error().Code, INVALID_PARAMS);
        const rpc::Result bad =
            window->Call("session.close", QJsonObject{{"channel", "1"}});
        QVERIFY(!bad);
        QCOMPARE(bad.error().Code, INVALID_PARAMS);

        QVERIFY(
            window->Call("session.close",
                         QJsonObject{{"channel", static_cast<int>(channel)}}));
        const rpc::Result twice =
            window->Call("session.close",
                         QJsonObject{{"channel", static_cast<int>(channel)}});
        QVERIFY(!twice);
        QCOMPARE(twice.error().Code, INVALID_PARAMS);
    }

    void AnExitIsToldToTheChannel() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        const std::uint32_t channel = window->Open("s1");
        QVERIFY(channel);
        QVERIFY(window->WaitDump(channel, "answers da1=1 cpr=1"));
        window->Rpc.SendInput(channel, "e");
        QTRY_COMPARE_WITH_TIMEOUT(window->Closed.size(), 1, TIMEOUT_MS);
        QCOMPARE(
            window->Closed.at(0),
            (QJsonObject{{"channel", 1}, {"reason", "exited with code 7"}}));
        QCOMPARE(Channels_->ChannelCount(), 0);
        QCOMPARE(Channels_->Pool().Count(), 0);

        const rpc::Result resized = window->Call(
            "session.resize",
            QJsonObject{{"channel", 1}, {"columns", 90}, {"rows", 20}});
        QVERIFY(!resized);
        QCOMPARE(resized.error().Code, INVALID_PARAMS);
        // Only the exit is told, and once.
        QTest::qWait(50);
        QCOMPARE(window->Closed.size(), 1);
    }

    void AnEvictionIsToldToTheChannel() {
        QVERIFY(Start({.Capacity = 1, .Pty = {}}));
        Window* window = Connect();
        QVERIFY(window);
        const std::uint32_t first = window->Open("s1");
        QVERIFY(first);
        QVERIFY(window->Open("s2"));
        QTRY_COMPARE_WITH_TIMEOUT(window->Closed.size(), 1, TIMEOUT_MS);
        QCOMPARE(window->Closed.at(0),
                 (QJsonObject{{"channel", 1}, {"reason", "evicted"}}));
        QCOMPARE(Channels_->ChannelCount(), 1);
        QCOMPARE(Channels_->Pool().Find(Key("s1")), nullptr);
        QVERIFY(Channels_->Pool().Find(Key("s2")));
    }

    void ARemovedWarmSessionIsClosedWithReasonRemoved() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        QVERIFY(window->Open("s1"));
        QVERIFY(window->Open("s2"));
        Window* second = Connect();
        QVERIFY(second);
        QVERIFY(second->Open("s1"));
        QCOMPARE(Channels_->ChannelCount(), 3);

        QVERIFY(Server_->Bus().Publish(
            "session.removed", QJsonObject{{"agent", "fake"}, {"id", "s1"}}));

        // Every channel of the session is told, with the reason "removed".
        QTRY_COMPARE_WITH_TIMEOUT(window->Closed.size(), 1, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(second->Closed.size(), 1, TIMEOUT_MS);
        QCOMPARE(window->Closed.at(0),
                 (QJsonObject{{"channel", 1}, {"reason", "removed"}}));
        QCOMPARE(second->Closed.at(0),
                 (QJsonObject{{"channel", 1}, {"reason", "removed"}}));
        QCOMPARE(Channels_->ChannelCount(), 1);
        QCOMPARE(Channels_->Pool().Count(), 1);
        QCOMPARE(Channels_->Pool().Find(Key("s1")), nullptr);
        QVERIFY(!Channels_->Pool().Find(Key("s2"))->IsClosed());
    }

    void ARemovalOfAnotherAgentsSessionWithTheSameIdKeepsIt() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        const std::uint32_t fake = window->Open("s1");
        QVERIFY(fake);
        const rpc::Result other = window->OpenWith(
            QJsonObject{{"agent", "other"}, {"session", "s1"}});
        QVERIFY2(other, qPrintable(Why(other)));
        const int otherChannel = other->toObject().value("channel").toInt();
        QVERIFY(otherChannel > 0);
        QCOMPARE(Channels_->Pool().Count(), 2);

        QVERIFY(Server_->Bus().Publish(
            "session.removed", QJsonObject{{"agent", "other"}, {"id", "s1"}}));

        QTRY_COMPARE_WITH_TIMEOUT(window->Closed.size(), 1, TIMEOUT_MS);
        QCOMPARE(window->Closed.at(0), (QJsonObject{{"channel", otherChannel},
                                                    {"reason", "removed"}}));
        QTest::qWait(NOTHING_HAPPENS_MS);
        QCOMPARE(window->Closed.size(), 1);
        QCOMPARE(Channels_->Pool().Count(), 1);
        QCOMPARE(Channels_->ChannelCount(), 1);
        QVERIFY(Channels_->Pool().Find(Key("s1")));
        QVERIFY(!Channels_->Pool().Find(Key("s1"))->IsClosed());
        QVERIFY(window->WaitDump(fake, "answers da1=1 cpr=1"));
    }

    void ARemovalOfANonWarmSessionChangesNothing() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        QVERIFY(window->Open("s1"));
        QCOMPARE(Channels_->Pool().Count(), 1);

        // A session that is not warm, an agent that is not served, and
        // payloads that are not a removal at all.
        QVERIFY(Server_->Bus().Publish(
            "session.removed", QJsonObject{{"agent", "fake"}, {"id", "s3"}}));
        QVERIFY(Server_->Bus().Publish(
            "session.removed", QJsonObject{{"agent", "nobody"}, {"id", "s1"}}));
        QVERIFY(Server_->Bus().Publish("session.removed", QJsonObject{}));
        QVERIFY(Server_->Bus().Publish("session.removed", QJsonValue()));
        QVERIFY(Server_->Bus().Publish(
            "session.removed", QJsonObject{{"agent", 1}, {"id", "s1"}}));

        QTest::qWait(NOTHING_HAPPENS_MS);
        QCOMPARE(Channels_->Pool().Count(), 1);
        QCOMPARE(Channels_->ChannelCount(), 1);
        QVERIFY(window->Closed.isEmpty());
        QVERIFY(!Channels_->Pool().Find(Key("s1"))->IsClosed());
    }

    void ARemovalAfterTheChannelsAreGoneIsIgnored() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        QVERIFY(window->Open("s1"));
        const std::size_t subscriptions = Server_->Bus().SubscriptionCount();

        // The Server and its bus outlive the channels: the subscription to
        // session.removed must not.
        Channels_.reset();
        QCOMPARE(Server_->Bus().SubscriptionCount(), subscriptions - 1);

        QVERIFY(Server_->Bus().Publish(
            "session.removed", QJsonObject{{"agent", "fake"}, {"id", "s1"}}));
        QTest::qWait(NOTHING_HAPPENS_MS);
        QVERIFY(window->Closed.isEmpty());
    }

    void AClosedConnectionTakesItsChannelsAndLeavesTheSessionWarm() {
        QVERIFY(Start());
        Window* gone = Connect();
        Window* stays = Connect();
        QVERIFY(gone && stays);
        const std::uint32_t channel = gone->Open("s1");
        QVERIFY(channel);
        QVERIFY(gone->Open("s2"));
        const std::uint32_t kept = stays->Open("s1");
        QVERIFY(kept);
        QCOMPARE(Channels_->ChannelCount(), 3);

        gone->Rpc.Disconnect();
        QTRY_COMPARE_WITH_TIMEOUT(Channels_->ChannelCount(), 1, TIMEOUT_MS);
        QCOMPARE(Channels_->Pool().Count(), 2);
        QVERIFY(!Channels_->Pool().Find(Key("s1"))->IsClosed());
        QVERIFY(!Channels_->Pool().Find(Key("s2"))->IsClosed());

        // The other connection's channel still works.
        QVERIFY(stays->WaitDump(kept, "answers da1=1 cpr=1"));
        stays->Rpc.SendInput(kept, "q");
        QVERIFY(stays->WaitDump(kept, "answers da1=2 cpr=2"));
    }

    void AChannelOfAnotherConnectionIsNotThere() {
        QVERIFY(Start());
        Window* owner = Connect();
        Window* stranger = Connect();
        QVERIFY(owner && stranger);
        const std::uint32_t channel = owner->Open("s1");
        QCOMPARE(channel, 1U);
        QVERIFY(owner->WaitDump(channel, "answers da1=1 cpr=1"));

        const QJsonObject resize{{"channel", 1}, {"columns", 90}, {"rows", 20}};
        const rpc::Result resized = stranger->Call("session.resize", resize);
        QVERIFY(!resized);
        QCOMPARE(resized.error().Code, INVALID_PARAMS);
        const rpc::Result closed =
            stranger->Call("session.close", QJsonObject{{"channel", 1}});
        QVERIFY(!closed);
        QCOMPARE(closed.error().Code, INVALID_PARAMS);
        QCOMPARE(Channels_->Pool().Find(Key("s1"))->Size(), PtySize{});
        QCOMPARE(Channels_->ChannelCount(), 1);

        // x from the stranger would end the program; q after it, from the
        // owner, finds it alive. The input of the stranger is read first.
        stranger->Rpc.SendInput(1, "x");
        owner->Rpc.SendInput(channel, "q");
        QVERIFY2(owner->WaitDump(channel, "answers da1=2 cpr=2"),
                 owner->Dump(channel).constData());
        QVERIFY(!Channels_->Pool().Find(Key("s1"))->IsClosed());

        // A number of its own in the stranger's table is its own channel.
        QCOMPARE(stranger->Open("s2"), 1U);
        QVERIFY(stranger->WaitDump(1, "answers da1=1 cpr=1"));
        stranger->Rpc.SendInput(1, "q");
        QVERIFY(stranger->WaitDump(1, "answers da1=2 cpr=2"));
        // The owner's program did not get the stranger's q: it would ask
        // again and count a third answer. q draws no input row, so the
        // answers are what tells; a leaked q may arrive late, hence the
        // window.
        QTest::qWait(NOTHING_HAPPENS_MS);
        const QByteArray ownerScreen = owner->Dump(1);
        QVERIFY2(ownerScreen.contains("answers da1=2 cpr=2"),
                 ownerScreen.constData());
    }

    void InputForAnUnknownChannelIsDropped() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        const std::uint32_t channel = window->Open("s1");
        QVERIFY(channel);
        QVERIFY(window->WaitDump(channel, "answers da1=1 cpr=1"));
        window->Rpc.SendInput(0, "x");
        window->Rpc.SendInput(7, "x");
        window->Rpc.SendInput(channel, "q");
        QVERIFY(window->WaitDump(channel, "answers da1=2 cpr=2"));
        QVERIFY(window->Rpc.IsConnected());
        QVERIFY(!Channels_->Pool().Find(Key("s1"))->IsClosed());
    }

    void OpeningFails_data() {
        QTest::addColumn<QString>("agent");
        QTest::addColumn<QString>("session");
        QTest::addColumn<int>("code");
        QTest::addColumn<QString>("message");
        QTest::newRow("unknown agent") << "nobody" << "s1" << INVALID_PARAMS
                                       << "no plugin serves agent nobody";
        QTest::newRow("cannot be attached to")
            << "fake" << "null" << INVALID_PARAMS
            << "session null of agent fake cannot be attached to";
        QTest::newRow("the adapter's error")
            << "fake" << "broken" << 5 << "no way";
        QTest::newRow("the program does not start")
            << "fake" << "missing" << INTERNAL_ERROR << "";
    }

    void OpeningFails() {
        QFETCH(QString, agent);
        QFETCH(QString, session);
        QFETCH(int, code);
        QFETCH(QString, message);
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        const rpc::Result opened = window->OpenWith(
            QJsonObject{{"agent", agent}, {"session", session}});
        QVERIFY(!opened);
        QCOMPARE(opened.error().Code, code);
        if (message.isEmpty()) {
            QVERIFY(!opened.error().Message.isEmpty());
        } else {
            QCOMPARE(opened.error().Message, message);
        }
        QCOMPARE(window->Log, QStringList{QString("error %1").arg(code)});
        QCOMPARE(Channels_->ChannelCount(), 0);
        QCOMPARE(Channels_->Pool().Count(), 0);
        // The connection is fine, and the next open gets channel 1.
        QCOMPARE(window->Open("s1"), 1U);
    }

    void BadOpenParametersAreInvalidParams_data() {
        QTest::addColumn<QJsonValue>("params");
        const auto with = [](const QString& key, const QJsonValue& value) {
            QJsonObject params{{"agent", "fake"}, {"session", "s1"}};
            params.insert(key, value);
            return QJsonValue(params);
        };
        QTest::newRow("no params") << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("array") << QJsonValue(QJsonArray{"fake", "s1"});
        QTest::newRow("no agent") << QJsonValue(QJsonObject{{"session", "s1"}});
        QTest::newRow("no session")
            << QJsonValue(QJsonObject{{"agent", "fake"}});
        QTest::newRow("agent as number") << with("agent", 1);
        QTest::newRow("session as number") << with("session", 1);
        QTest::newRow("zero columns") << with("columns", 0);
        QTest::newRow("zero rows") << with("rows", 0);
        QTest::newRow("too many columns") << with("columns", 2001);
        QTest::newRow("too many rows") << with("rows", 2001);
        QTest::newRow("negative rows") << with("rows", -1);
        QTest::newRow("fractional columns") << with("columns", 80.5);
        QTest::newRow("columns as string") << with("columns", "80");
        QTest::newRow("null rows") << with("rows", QJsonValue());
        QTest::newRow("only columns") << with("columns", 100);
        QTest::newRow("only rows") << with("rows", 30);
    }

    void BadOpenParametersAreInvalidParams() {
        QFETCH(QJsonValue, params);
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        const rpc::Result opened = window->OpenWith(params);
        QVERIFY(!opened);
        QCOMPARE(opened.error().Code, INVALID_PARAMS);
        QCOMPARE(Channels_->ChannelCount(), 0);
        QCOMPARE(Channels_->Pool().Count(), 0);
    }

    void TheLargestSizeIsOpened() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        const std::uint32_t channel =
            window->Open("s1", {{"columns", 2000}, {"rows", 1}});
        QVERIFY2(channel, qPrintable(Why(window->LastOpen)));
        QCOMPARE(Channels_->Pool().Find(Key("s1"))->Size(),
                 (PtySize{.Columns = 2000, .Rows = 1}));
    }

    void AClosedChannelGetsNoMoreOutput() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        const std::uint32_t closed = window->Open("s1");
        QVERIFY(closed);
        QVERIFY(window->WaitDump(closed, "answers da1=1 cpr=1"));
        const rpc::Result result =
            window->Call("session.close",
                         QJsonObject{{"channel", static_cast<int>(closed)}});
        QVERIFY2(result, qPrintable(Why(result)));
        const qsizetype outputs =
            window->Log.count(QString("output %1").arg(closed));

        // The session prints again, as the answer to a key on another channel.
        const std::uint32_t other = window->Open("s1");
        QVERIFY2(other, qPrintable(Why(window->LastOpen)));
        QVERIFY(other != closed);
        window->Rpc.SendInput(other, "q");
        QVERIFY2(window->WaitDump(other, "answers da1=2 cpr=2"),
                 window->Dump(other).constData());
        QTest::qWait(50);
        QCOMPARE(window->Log.count(QString("output %1").arg(closed)), outputs);
    }

    void AConnectionClosedDuringTheAttachOpensNothing() {
        QVERIFY(Start());
        Window* gone = Connect();
        Window* stays = Connect();
        QVERIFY(gone && stays);
        // The backend is up and answers: this one is turned down.
        QVERIFY(!stays->OpenWith(
            QJsonObject{{"agent", "fake"}, {"session", "null"}}));

        // The client leaves before the adapter answers: the adapter sits on
        // the answer, and the server sees the connection closed meanwhile.
        QSignalSpy closed(Server_.get(), &Server::ConnectionClosed);
        const QFuture<rpc::Result> pending = gone->Rpc.Request(
            "session.open",
            QJsonObject{{"agent", "fake"}, {"session", "slow"}});
        Q_UNUSED(pending);
        // The server answers in the order it reads: once it has answered this,
        // it has taken the open and asked the adapter.
        QVERIFY(!gone->Call("session.close", QJsonObject{{"channel", 99}}));
        gone->Rpc.Disconnect();
        QVERIFY(closed.wait(TIMEOUT_MS));

        // The adapter answers one request at a time, in order: this turn-down
        // comes after the answer to the open of the gone window, and the
        // server completes the answers in the order they come.
        QVERIFY(!stays->OpenWith(
            QJsonObject{{"agent", "fake"}, {"session", "null"}}));
        QCOMPARE(Channels_->ChannelCount(), 0);
        QCOMPARE(Channels_->Pool().Count(), 0);
    }

    void InputAndOpeningCountAsViewing_data() {
        QTest::addColumn<bool>("byInput");
        QTest::newRow("input") << true;
        QTest::newRow("opening again") << false;
    }

    void InputAndOpeningCountAsViewing() {
        QFETCH(bool, byInput);
        QVERIFY(Start({.Capacity = 2, .Pty = {}}));
        Window* window = Connect();
        QVERIFY(window);
        const std::uint32_t first = window->Open("s1");
        const std::uint32_t second = window->Open("s2");
        QVERIFY(first && second);
        QVERIFY(window->WaitDump(first, "answers da1=1 cpr=1"));
        if (byInput) {
            window->Rpc.SendInput(first, "z");
            QVERIFY2(window->WaitDump(first, "input z total=1"),
                     window->Dump(first).constData());
        } else {
            QVERIFY(window->Open("s1"));
        }

        // s2 was viewed the longest ago.
        QVERIFY(window->Open("s3"));
        QTRY_COMPARE_WITH_TIMEOUT(window->Closed.size(), 1, TIMEOUT_MS);
        QCOMPARE(window->Closed.at(0),
                 (QJsonObject{{"channel", static_cast<int>(second)},
                              {"reason", "evicted"}}));
        QVERIFY(Channels_->Pool().Find(Key("s1")));
        QCOMPARE(Channels_->Pool().Find(Key("s2")), nullptr);
        QVERIFY(Channels_->Pool().Find(Key("s3")));
    }

    void AViewKeepsASessionFromEviction() {
        QVERIFY(Start({.Capacity = 2, .Pty = {}}));
        Window* window = Connect();
        QVERIFY(window);
        const std::uint32_t first = window->Open("s1");
        const std::uint32_t second = window->Open("s2");
        QVERIFY(first && second);

        const rpc::Result viewed = window->Call(
            "session.view", QJsonObject{{"channel", static_cast<int>(first)}});
        QVERIFY(viewed);
        QVERIFY(viewed->isNull());

        // s2 was viewed the longest ago.
        QVERIFY(window->Open("s3"));
        QTRY_COMPARE_WITH_TIMEOUT(window->Closed.size(), 1, TIMEOUT_MS);
        QCOMPARE(window->Closed.at(0),
                 (QJsonObject{{"channel", static_cast<int>(second)},
                              {"reason", "evicted"}}));
        QVERIFY(Channels_->Pool().Find(Key("s1")));
        QCOMPARE(Channels_->Pool().Find(Key("s2")), nullptr);
        QVERIFY(Channels_->Pool().Find(Key("s3")));
    }

    void AViewOfAnUnknownChannelIsInvalidParams() {
        QVERIFY(Start());
        Window* owner = Connect();
        Window* stranger = Connect();
        QVERIFY(owner && stranger);
        QCOMPARE(owner->Open("s1"), 1U);

        const auto expectInvalid = [](Window* window,
                                      const QJsonValue& params) {
            const rpc::Result result = window->Call("session.view", params);
            QVERIFY(!result);
            QCOMPARE(result.error().Code, INVALID_PARAMS);
        };
        expectInvalid(owner, QJsonObject{{"channel", 99}});
        expectInvalid(stranger, QJsonObject{{"channel", 1}});
        expectInvalid(owner, QJsonObject{});
        expectInvalid(owner, QJsonObject{{"channel", "1"}});
        QCOMPARE(Channels_->ChannelCount(), 1);
    }

    void AViewChangesNothingButTheViewingOrder() {
        QVERIFY(Start());
        Window* viewer = Connect();
        Window* other = Connect();
        QVERIFY(viewer && other);
        const std::uint32_t channel = viewer->Open("s1");
        const std::uint32_t shared = other->Open("s1");
        QVERIFY(channel && shared);
        // The program has said all it has to say.
        QVERIFY(viewer->WaitDump(channel, "answers da1=1 cpr=1"));
        QVERIFY(other->WaitDump(shared, "answers da1=1 cpr=1"));
        const qsizetype viewerLog = viewer->Log.size();
        const qsizetype otherLog = other->Log.size();
        const PtySize size = Channels_->Pool().Find(Key("s1"))->Size();

        const rpc::Result viewed =
            viewer->Call("session.view",
                         QJsonObject{{"channel", static_cast<int>(channel)}});
        QVERIFY(viewed);
        QVERIFY(viewed->isNull());

        // No notification, frame or other change on either connection.
        QTest::qWait(NOTHING_HAPPENS_MS);
        QCOMPARE(viewer->Log.size(), viewerLog);
        QCOMPARE(other->Log.size(), otherLog);
        QVERIFY(viewer->Closed.isEmpty());
        QVERIFY(other->Closed.isEmpty());
        QCOMPARE(Channels_->Pool().Find(Key("s1"))->Size(), size);
        QCOMPARE(Channels_->ChannelCount(), 2);
        QVERIFY(!Channels_->Pool().Find(Key("s1"))->IsClosed());
    }

    void AViewOfAClosedChannelIsInvalidParams() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        const std::uint32_t channel = window->Open("s1");
        QVERIFY(channel);
        const QJsonObject params{{"channel", static_cast<int>(channel)}};
        QVERIFY(window->Call("session.view", params));
        QVERIFY(window->Call("session.close", params));

        const rpc::Result viewed = window->Call("session.view", params);
        QVERIFY(!viewed);
        QCOMPARE(viewed.error().Code, INVALID_PARAMS);
        QCOMPARE(Channels_->ChannelCount(), 0);
        QVERIFY(Channels_->Pool().Find(Key("s1")));
    }

    // The list a restarted window prefetches from: most recently viewed
    // first, and a view reorders it.
    void WarmSessionsAreListedMostRecentFirst() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        const std::uint32_t first = window->Open("s1");
        QVERIFY(first);
        QVERIFY(window->Open("s2"));
        QCOMPARE(WarmList(*window), (QStringList{"fake/s2", "fake/s1"}));

        QVERIFY(window->Call(
            "session.view", QJsonObject{{"channel", static_cast<int>(first)}}));

        QCOMPARE(WarmList(*window), (QStringList{"fake/s1", "fake/s2"}));
        // Asking does not change what it answers.
        QCOMPARE(WarmList(*window), (QStringList{"fake/s1", "fake/s2"}));
    }

    void AnEmptyPoolListsNoWarmSessions() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);

        const rpc::Result warm = window->Call("sessions.warm");

        QVERIFY2(warm, qPrintable(Why(warm)));
        QCOMPARE(*warm, QJsonValue(QJsonObject{{"sessions", QJsonArray()}}));
    }

    void OutputDuringAnOpenIsNeitherLostNorRepeated() {
        QVERIFY(Start());
        Window* first = Connect();
        Window* second = Connect();
        QVERIFY(first && second);
        const std::uint32_t one = first->Open("stream");
        QVERIFY2(one, qPrintable(Why(first->LastOpen)));
        // The program is printing: the second window comes in the middle.
        QVERIFY(QTest::qWaitFor(
            [&] { return first->Log.count("output 1") > 0; }, TIMEOUT_MS));
        const std::uint32_t other = second->Open("stream");
        QVERIFY2(other, qPrintable(Why(second->LastOpen)));
        QVERIFY(first->WaitDump(one, "done"));
        QVERIFY(second->WaitDump(other, "done"));

        const QByteArray daemon = DaemonDump(*Channels_, "stream");
        QCOMPARE(first->Dump(one), daemon);
        QCOMPARE(second->Dump(other), daemon);
    }

    void WithoutTheChannelsTheMethodsFail() {
        QVERIFY(Start());
        Window* window = Connect();
        QVERIFY(window);
        Channels_.reset();
        const rpc::Result opened =
            window->OpenWith(QJsonObject{{"agent", "fake"}, {"session", "s1"}});
        QVERIFY(!opened);
        QCOMPARE(opened.error().Code, INTERNAL_ERROR);
        const rpc::Result closed =
            window->Call("session.close", QJsonObject{{"channel", 1}});
        QVERIFY(!closed);
        QCOMPARE(closed.error().Code, INTERNAL_ERROR);
    }
};

QTEST_GUILESS_MAIN(SessionChannelsTest)
#include "session_channels_test.moc"
