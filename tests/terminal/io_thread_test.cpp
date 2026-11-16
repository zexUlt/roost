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

#include "core/rpc_client.hpp"
#include "terminal/io_thread.hpp"

using namespace roost;

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
        Daemon_->setProcessChannelMode(QProcess::ForwardedErrorChannel);
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
        int updates = 0;
        connect(io.get(), &IoThread::Updated, this, [&updates] { ++updates; });
        QVERIFY(OpenAndSettle(*io));
        // Let what is in flight arrive, and start from an acknowledged state.
        QTest::qWait(300);
        io->AcknowledgeUpdate(Opened_.Channel);
        QTest::qWait(100);
        updates = 0;
        const int before = AnswersSeen(Opened_);

        // Each q makes the TUI ask again and redraw: many output frames.
        constexpr int keys = 20;
        for (int i = 0; i < keys; ++i) {
            io->SendInput(Opened_.Channel, "q");
            QTest::qWait(10);
        }
        QTRY_VERIFY_WITH_TIMEOUT(AnswersSeen(Opened_) >= before + keys / 2,
                                 TIMEOUT_MS);
        QTest::qWait(300);
        QCOMPARE(updates, 1);

        io->AcknowledgeUpdate(Opened_.Channel);
        io->SendInput(Opened_.Channel, "q");
        QTRY_COMPARE_WITH_TIMEOUT(updates, 2, TIMEOUT_MS);
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
