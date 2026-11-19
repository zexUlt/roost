// The window's IO thread against the real roostd with the real Claude plugin,
// behind a fake `claude` whose `attach` runs fake_tui: what the terminal of an
// opened session shows, that input and a resize reach the PTY, that output is
// written in the IO thread and its signal is coalesced, and that stopping with
// work in flight neither crashes nor hangs.

#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <signal.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "core/frame.hpp"
#include "core/protocol.hpp"
#include "core/rpc_client.hpp"
#include "helpers/process_lifeline.hpp"
#include "terminal/io_thread.hpp"

using namespace roost;
using roost::testing::ProcessLifeline;
using roost::testing::RelayStandardError;

namespace {

constexpr int TIMEOUT_MS = 10000;
constexpr const char* SESSION_ID = "bg-1b2c3d4e";
constexpr quint16 COLUMNS = 80;
constexpr quint16 ROWS = 24;
// Each query answered once; a digit after the 1 would be more than once.
const QRegularExpression ANSWERED_ONCE(
    QStringLiteral(R"(answers da1=1 cpr=1(?!\d))"));
const QRegularExpression ANSWER_COUNT(QStringLiteral(R"(answers da1=(\d+))"));

// A fake `claude`, steered by the files next to it: agents.json is what
// `agents` prints, and `attach <id>` runs fake_tui.
constexpr const char* FAKE_CLAUDE = R"sh(#!/bin/sh
dir=$(dirname "$0")
case "$1" in
agents) cat "$dir/agents.json" ;;
attach) exec "$dir/fake_tui" ;;
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
bool WaitFor(const QFuture<TValue>& future, int timeoutMs = TIMEOUT_MS) {
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

QString Why(const OpenResult& result) {
    return result ? QString() : result.error().Message;
}

// What the terminal shows, as the VT sequences that rebuild it.
QString Screen(const OpenedSession& opened) {
    const auto lock = opened.Session->Lock();
    return QString::fromUtf8(
        opened.Session->Dump(lock).value_or("dump failed"));
}

// Probes the terminal width through its dump: moves the cursor to column 999,
// where it clamps to the last column, and returns the dump, which carries the
// cursor position.
QString ProbeColumn(const OpenedSession& opened) {
    const auto lock = opened.Session->Lock();
    opened.Session->Write(lock, "\x1b[1;999H");
    return QString::fromUtf8(
        opened.Session->Dump(lock).value_or("dump failed"));
}

int AnswersSeen(const OpenedSession& opened) {
    const auto match = ANSWER_COUNT.match(Screen(opened));
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

// A stand-in for roostd that answers every request with an empty result,
// session.open with channel 7, and sends nothing else.
class AnswersOnlyServer : public QObject {
public:
    bool Listen(const QString& path) {
        Path_ = path;
        connect(&Server_, &QLocalServer::newConnection, this, [this] {
            Socket_ = Server_.nextPendingConnection();
            connect(Socket_, &QLocalSocket::readyRead, this,
                    [this] { OnReadable(); });
        });
        return Server_.listen(path);
    }

    const QString& Path() const { return Path_; }

    bool Answered(const QString& method) const {
        return Answered_.contains(method);
    }

private:
    void OnReadable() {
        Decoder_.Feed(Socket_->readAll());
        while (const auto frame = Decoder_.Next()) {
            if (frame->Kind != EFrameKind::Rpc) {
                continue;
            }
            const QJsonObject request =
                QJsonDocument::fromJson(frame->Body).object();
            const QString method = request.value("method").toString();
            const QJsonValue result =
                method == QStringLiteral("session.open")
                    ? QJsonValue(QJsonObject{{"channel", 7}})
                    : QJsonValue(QJsonObject{});
            const QJsonObject answer{{"jsonrpc", "2.0"},
                                     {"id", request.value("id")},
                                     {"result", result}};
            Socket_->write(
                EncodeFrame(EFrameKind::Rpc, QJsonDocument(answer).toJson(
                                                 QJsonDocument::Compact))
                    .value());
            Answered_ << method;
        }
    }

    QLocalServer Server_;
    QLocalSocket* Socket_ = nullptr;
    FrameDecoder Decoder_;
    QString Path_;
    QStringList Answered_;
};

// Stops the process while alive and lets it go on at the end of the scope, so
// that a failing check does not leave a stopped daemon for cleanup to wait on.
class Paused {
public:
    explicit Paused(QProcess& process)
        : Pid_(static_cast<pid_t>(process.processId())) {
        ::kill(Pid_, SIGSTOP);
    }
    ~Paused() { Resume(); }
    Paused(const Paused&) = delete;
    Paused& operator=(const Paused&) = delete;

    void Resume() {
        if (Pid_ > 0) {
            ::kill(Pid_, SIGCONT);
            Pid_ = 0;
        }
    }

private:
    pid_t Pid_;
};

// Counts the Updated signals in the thread that emits them, so the count is
// exact at any moment and does not wait for the GUI event loop.
std::atomic<int>& CountUpdates(const IoThread& io, std::atomic<int>& counter) {
    QObject::connect(
        &io, &IoThread::Updated, &io, [&counter] { ++counter; },
        Qt::DirectConnection);
    return counter;
}

int Code(const rpc::Result& result) { return result ? 0 : result.error().Code; }

constexpr int CONNECTION_LOST =
    static_cast<int>(protocol::EErrorCode::ConnectionLost);
// How long to watch for something that must not happen.
constexpr int QUIET_MS = 500;

}  // namespace

class IoThreadTest : public QObject {
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

    bool LaunchDaemon() {
        if (!WriteFakeClaude()) {
            return false;
        }
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("ROOST_SOCKET", SocketPath());
        environment.insert("ROOST_CONFIG", Dir_->filePath("config"));
        environment.insert("ROOST_PLUGINS_PATH", ROOST_PLUGINS_DIR);
        environment.insert(
            "PATH", Dir_->filePath("bin") + u':' + environment.value("PATH"));
        Daemon_ = std::make_unique<QProcess>();
        Daemon_->setProcessEnvironment(environment);
        RelayStandardError(*Daemon_);
        if (!Lifeline_.Attach(*Daemon_)) {
            return false;
        }
        Daemon_->start(ROOSTD_PATH, {});
        return Daemon_->waitForStarted(TIMEOUT_MS) && Greets(SocketPath());
    }

    // The IO thread, connected to the daemon.
    std::unique_ptr<IoThread> ConnectedIo() {
        auto io = std::make_unique<IoThread>();
        const QFuture<rpc::Result> hello = io->Connect(SocketPath());
        if (!WaitFor(hello) || !hello.result()) {
            return nullptr;
        }
        return io;
    }

    // Opens the session and waits until its TUI has had its queries answered:
    // fake_tui reacts to a key only if the key arrives in a read of its own.
    // The result stays in the member; false if anything failed.
    bool OpenAndSettle(IoThread& io, const QString& session = SESSION_ID) {
        const QFuture<OpenResult> opening =
            io.Open("claude", session, COLUMNS, ROWS);
        if (!WaitFor(opening)) {
            return false;
        }
        const OpenResult& result = opening.result();
        if (!result) {
            qWarning("open failed: %s", qPrintable(Why(result)));
            return false;
        }
        Opened_ = *result;
        return QTest::qWaitFor(
            [&] { return Screen(Opened_).contains(ANSWERED_ONCE); },
            TIMEOUT_MS);
    }

    std::unique_ptr<QTemporaryDir> Dir_;
    ProcessLifeline Lifeline_;
    std::unique_ptr<QProcess> Daemon_;
    OpenedSession Opened_;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
        Opened_ = {};
        QVERIFY(LaunchDaemon());
    }

    void cleanup() {
        Opened_ = {};
        if (Daemon_ && Daemon_->state() != QProcess::NotRunning) {
            QCOMPARE(::kill(static_cast<pid_t>(Daemon_->processId()), SIGTERM),
                     0);
            QVERIFY(Daemon_->waitForFinished(TIMEOUT_MS));
            QCOMPARE(Daemon_->exitStatus(), QProcess::NormalExit);
            QCOMPARE(Daemon_->exitCode(), 0);
        }
        Daemon_.reset();
    }

    void OpenGivesASessionThatShowsTheTui() {
        const auto io = ConnectedIo();
        QVERIFY(io);
        const QFuture<OpenResult> opening =
            io->Open("claude", SESSION_ID, COLUMNS, ROWS);
        QVERIFY(WaitFor(opening));
        const OpenResult& result = opening.result();
        QVERIFY2(result, qPrintable(Why(result)));
        QVERIFY(result->Channel > 0);
        QVERIFY(result->Session);

        QTRY_VERIFY_WITH_TIMEOUT(Screen(*result).contains("fake tui cwd=") &&
                                     Screen(*result).contains(ANSWERED_ONCE),
                                 TIMEOUT_MS);
    }

    void InputReachesTheTuiAndItsEchoIsDrawn() {
        const auto io = ConnectedIo();
        QVERIFY(io);
        QVERIFY(OpenAndSettle(*io));

        io->SendInput(Opened_.Channel, "hello");
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Opened_).contains("input hello total=5"), TIMEOUT_MS);
    }

    void AResizeReachesThePtyAndTheLocalTerminal() {
        const auto io = ConnectedIo();
        QVERIFY(io);
        QVERIFY(OpenAndSettle(*io));
        // The cursor sent past the right edge stops at the last column.
        QVERIFY(ProbeColumn(Opened_).endsWith("\x1b[1;80H\x1b[0m"));

        const QFuture<rpc::Result> resizing =
            io->Resize(Opened_.Channel, 100, 30);
        QVERIFY(WaitFor(resizing));
        const rpc::Result& resized = resizing.result();
        QVERIFY2(resized, qPrintable(Why(resized)));
        QVERIFY(ProbeColumn(Opened_).endsWith("\x1b[1;100H\x1b[0m"));
        QTRY_VERIFY_WITH_TIMEOUT(Screen(Opened_).contains("size 100x30"),
                                 TIMEOUT_MS);
    }

    void ClosedArrivesWhenTheTuiExits() {
        const auto io = ConnectedIo();
        QVERIFY(io);
        std::vector<std::pair<std::uint32_t, QString>> closed;
        connect(io.get(), &IoThread::Closed, this,
                [&closed](std::uint32_t channel, const QString& reason) {
                    closed.emplace_back(channel, reason);
                });
        QVERIFY(OpenAndSettle(*io));

        io->SendInput(Opened_.Channel, "x");
        QTRY_VERIFY_WITH_TIMEOUT(!closed.empty(), TIMEOUT_MS);
        QCOMPARE(closed.front().first, Opened_.Channel);
        QCOMPARE(closed.front().second, QStringLiteral("exited with code 0"));
    }

    void OutputIsWrittenInTheIoThread() {
        const auto io = ConnectedIo();
        QVERIFY(io);
        std::atomic<QThread*> updatedIn{nullptr};
        // Direct: the slot runs in the thread that emits.
        connect(
            io.get(), &IoThread::Updated, this,
            [&updatedIn] { updatedIn = QThread::currentThread(); },
            Qt::DirectConnection);
        QVERIFY(OpenAndSettle(*io));
        io->AcknowledgeUpdate(Opened_.Channel);
        updatedIn = nullptr;

        io->SendInput(Opened_.Channel, "q");
        QTRY_VERIFY_WITH_TIMEOUT(updatedIn.load() != nullptr, TIMEOUT_MS);
        QVERIFY(updatedIn.load() != QThread::currentThread());
        QCOMPARE(updatedIn.load()->objectName(), QStringLiteral("roost-io"));
    }

    void UpdatedIsCoalescedUntilAcknowledged() {
        const auto io = ConnectedIo();
        QVERIFY(io);
        std::atomic<int> updates{0};
        CountUpdates(*io, updates);
        QVERIFY(OpenAndSettle(*io));
        io->AcknowledgeUpdate(Opened_.Channel);
        const int before = AnswersSeen(Opened_);

        // Each q makes the TUI ask again and redraw: many output frames.
        constexpr int keys = 20;
        for (int i = 0; i < keys; ++i) {
            io->SendInput(Opened_.Channel, "q");
            QTRY_VERIFY_WITH_TIMEOUT(AnswersSeen(Opened_) >= before + i + 1,
                                     TIMEOUT_MS);
        }
        QCOMPARE(updates.load(), 1);

        io->AcknowledgeUpdate(Opened_.Channel);
        io->SendInput(Opened_.Channel, "q");
        QTRY_COMPARE_WITH_TIMEOUT(updates.load(), 2, TIMEOUT_MS);
    }

    void OutputRightAfterOpenRaisesNoUpdatedUntilTheFirstAcknowledge() {
        const auto io = ConnectedIo();
        QVERIFY(io);
        std::atomic<int> updates{0};
        CountUpdates(*io, updates);
        // The TUI's first output (its queries answered) lands after Open.
        QVERIFY(OpenAndSettle(*io));
        QCOMPARE(updates.load(), 0);

        io->AcknowledgeUpdate(Opened_.Channel);
        io->SendInput(Opened_.Channel, "q");
        QTRY_COMPARE_WITH_TIMEOUT(updates.load(), 1, TIMEOUT_MS);
    }

    void ClosingAChannelDetachesItsTerminal() {
        const auto io = ConnectedIo();
        QVERIFY(io);
        std::atomic<int> updates{0};
        CountUpdates(*io, updates);
        QVERIFY(OpenAndSettle(*io));
        io->AcknowledgeUpdate(Opened_.Channel);

        const QFuture<rpc::Result> closing = io->Close(Opened_.Channel);
        QVERIFY(WaitFor(closing));
        const rpc::Result& closed = closing.result();
        QVERIFY2(closed, qPrintable(Why(closed)));

        const QString before = Screen(Opened_);
        io->SendInput(Opened_.Channel, "hello");
        // Proves that nothing happens: a fixed window.
        QTest::qWait(QUIET_MS);
        QCOMPARE(Screen(Opened_), before);
        QVERIFY(!Screen(Opened_).contains("input hello"));
        QCOMPARE(updates.load(), 0);
    }

    void AResizeRaisesUpdated() {
        const auto io = ConnectedIo();
        QVERIFY(io);
        std::atomic<int> updates{0};
        CountUpdates(*io, updates);
        QVERIFY(OpenAndSettle(*io));
        io->AcknowledgeUpdate(Opened_.Channel);
        QCOMPARE(updates.load(), 0);

        // The size stays the same, so the TUI gets no SIGWINCH and draws
        // nothing: the only source of Updated is the resize itself.
        const QFuture<rpc::Result> resizing =
            io->Resize(Opened_.Channel, COLUMNS, ROWS);
        QVERIFY(WaitFor(resizing));
        const rpc::Result& resized = resizing.result();
        QVERIFY2(resized, qPrintable(Why(resized)));
        QCOMPARE(updates.load(), 1);
    }

    void AResizeOutOfRangeFailsWithoutReachingThePty() {
        const auto io = ConnectedIo();
        QVERIFY(io);
        QVERIFY(OpenAndSettle(*io));
        QVERIFY(Screen(Opened_).contains("size 80x24"));

        // A stopped daemon answers nothing: a request that was sent would
        // stay pending, one that failed up front finishes at once.
        {
            const Paused paused(*Daemon_);
            for (const auto& [columns, rows] :
                 {std::pair<quint16, quint16>{0, ROWS},
                  {COLUMNS, 0},
                  {2001, ROWS},
                  {COLUMNS, 2001}}) {
                const QFuture<rpc::Result> resizing =
                    io->Resize(Opened_.Channel, columns, rows);
                QVERIFY2(
                    WaitFor(resizing, 1000),
                    qPrintable(QStringLiteral("%1x%2").arg(columns).arg(rows)));
                QCOMPARE(Code(resizing.result()),
                         static_cast<int>(rpc::EErrorCode::InvalidParams));
            }
        }
        QVERIFY(ProbeColumn(Opened_).endsWith("\x1b[1;80H\x1b[0m"));
        // The PTY kept its size: a later redraw would show a new one.
        io->SendInput(Opened_.Channel, "q");
        QTRY_VERIFY_WITH_TIMEOUT(AnswersSeen(Opened_) >= 2, TIMEOUT_MS);
        QVERIFY(Screen(Opened_).contains("size 80x24"));
    }

    void StopFailsAnOpenWaitingForItsSnapshotWithConnectionLost() {
        // The real roostd sends the snapshot in the same pass as the answer,
        // so the window in which an Open waits for it cannot be held there: a
        // stand-in answers session.open and never sends the snapshot.
        AnswersOnlyServer server;
        QVERIFY(server.Listen(Dir_->filePath("fake.sock")));
        IoThread io;
        const QFuture<rpc::Result> hello = io.Connect(server.Path());
        QVERIFY(WaitFor(hello));
        QVERIFY(hello.result());

        const QFuture<OpenResult> opening =
            io.Open("claude", SESSION_ID, COLUMNS, ROWS);
        QTRY_VERIFY_WITH_TIMEOUT(server.Answered("session.open"), TIMEOUT_MS);
        // The answer has been sent; give the IO thread time to process it.
        QTest::qWait(QUIET_MS);
        QVERIFY(!opening.isFinished());

        io.Stop();
        QVERIFY(opening.isFinished());
        QCOMPARE(opening.resultCount(), 1);
        const OpenResult& result = opening.result();
        QVERIFY(!result);
        QCOMPARE(result.error().Code, CONNECTION_LOST);
    }

    void RoostdGoingAwayEmitsDisconnectedAndFailsPendingFutures() {
        const auto io = ConnectedIo();
        QVERIFY(io);
        QVERIFY(OpenAndSettle(*io));
        QStringList disconnected;
        connect(
            io.get(), &IoThread::Disconnected, this,
            [&disconnected](const QString& reason) { disconnected << reason; });

        QFuture<rpc::Result> resizing;
        QFuture<OpenResult> opening;
        {
            // Both requests stay unanswered until the daemon is gone.
            const Paused paused(*Daemon_);
            resizing = io->Resize(Opened_.Channel, 100, 30);
            opening = io->Open("claude", "bg-0a1b2c3d", COLUMNS, ROWS);
            QVERIFY(!WaitFor(resizing, 200));
            Daemon_->kill();
        }
        QVERIFY(Daemon_->waitForFinished(TIMEOUT_MS));
        QVERIFY(WaitFor(resizing));
        QVERIFY(WaitFor(opening));
        QTRY_COMPARE_WITH_TIMEOUT(disconnected.size(), 1, TIMEOUT_MS);
        QCOMPARE(Code(resizing.result()), CONNECTION_LOST);
        QVERIFY(!opening.result());
        QCOMPARE(opening.result().error().Code, CONNECTION_LOST);
    }

    void DestroyingWithAChannelAndPendingRequestsNeitherCrashesNorHangs() {
        QFuture<OpenResult> opening;
        QFuture<rpc::Result> resizing;
        {
            const auto io = ConnectedIo();
            QVERIFY(io);
            QVERIFY(OpenAndSettle(*io));
            opening = io->Open("claude", "bg-0a1b2c3d", COLUMNS, ROWS);
            resizing = io->Resize(Opened_.Channel, 100, 30);
            io->SendInput(Opened_.Channel, "q");
        }
        // The futures are settled, with whichever answer came first.
        QVERIFY(opening.isFinished());
        QVERIFY(resizing.isFinished());
        QCOMPARE(opening.resultCount(), 1);
        QCOMPARE(resizing.resultCount(), 1);

        // A stopped thread answers at once, and fails.
        IoThread stopped;
        stopped.Stop();
        const QFuture<rpc::Result> late = stopped.Connect(SocketPath());
        QVERIFY(late.isFinished());
        QVERIFY(!late.result());
    }
};

QTEST_GUILESS_MAIN(IoThreadTest)
#include "io_thread_test.moc"
