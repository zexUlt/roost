// The developer mode against the real roostd with the real Claude plugin,
// behind a fake `claude` whose `attach` runs fake_tui: a TerminalItem in a
// window and the SingleSession controller. Typed keys reach the TUI and its
// echo is drawn; a resize of the item reaches the PTY; the TUI exiting ends
// the controller. Then the real `roost --session` as a process.

#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <signal.h>

#include <atomic>
#include <memory>

#include "core/protocol.hpp"
#include "core/rpc_client.hpp"
#include "helpers/process_lifeline.hpp"
#include "qml_api/terminal_item.hpp"
#include "roost/single_session.hpp"
#include "terminal/renderer.hpp"

using namespace roost;
using roost::testing::ProcessLifeline;
using roost::testing::RelayStandardError;

namespace {

constexpr int TIMEOUT_MS = 10000;
constexpr const char* SESSION_ID = "bg-1b2c3d4e";
constexpr int COLUMNS = 80;
constexpr int ROWS = 24;
// Each query answered once; a digit after the 1 would be more than once.
const QRegularExpression ANSWERED_ONCE(
    QStringLiteral(R"(answers da1=1 cpr=1(?!\d))"));

// What row 5 of the TUI shows once "world" has arrived, whatever the split. No
// 'e', 'q' or 'x': fake_tui takes them for commands when one arrives in a read
// of its own.
const QRegularExpression TYPED_WORLD(
    QStringLiteral(R"(input (?:d|ld|rld|orld|world) total=5(?!\d))"));

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

// What the terminal shows, as the VT sequences that rebuild it.
QString Screen(const std::shared_ptr<TerminalSession>& session) {
    if (!session) {
        return {};
    }
    const auto lock = session->Lock();
    return QString::fromUtf8(session->Dump(lock).value_or("dump failed"));
}

// Probes the terminal width through its dump: moves the cursor to column 999,
// where it clamps to the last column, and returns the dump, which carries the
// cursor position.
QString ProbeColumn(const std::shared_ptr<TerminalSession>& session) {
    const auto lock = session->Lock();
    session->Write(lock, "\x1b[1;999H");
    return QString::fromUtf8(session->Dump(lock).value_or("dump failed"));
}

QString SizeLine(int columns, int rows) {
    return QStringLiteral("size %1x%2").arg(columns).arg(rows);
}

}  // namespace

class SingleSessionTest : public QObject {
    Q_OBJECT

private:
    QString SocketPath() const { return Dir_->filePath("roostd.sock"); }

    QProcessEnvironment DaemonEnvironment() const {
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("ROOST_SOCKET", SocketPath());
        environment.insert("ROOST_CONFIG", Dir_->filePath("config"));
        environment.insert("ROOST_PLUGINS_PATH", ROOST_PLUGINS_DIR);
        environment.insert(
            "PATH", Dir_->filePath("bin") + u':' + environment.value("PATH"));
        return environment;
    }

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
        Daemon_ = std::make_unique<QProcess>();
        Daemon_->setProcessEnvironment(DaemonEnvironment());
        RelayStandardError(*Daemon_);
        if (!Lifeline_.Attach(*Daemon_)) {
            return false;
        }
        Daemon_->start(ROOSTD_PATH, {});
        return Daemon_->waitForStarted(TIMEOUT_MS) && Greets(SocketPath());
    }

    // A window with an item of exactly COLUMNS x ROWS cells, shown and with
    // the focus, and a controller started against the daemon.
    void ShowAndStart(const QString& session = SESSION_ID) {
        Window_ = std::make_unique<QQuickWindow>();
        Item_ = new TerminalItem(Window_->contentItem());
        const CellMetrics cell = MeasureCell(Item_->font());
        Window_->resize(COLUMNS * cell.Width, ROWS * cell.Height);
        Item_->setSize(QSizeF(Window_->size()));
        QCOMPARE(Item_->columns(), COLUMNS);
        QCOMPARE(Item_->rows(), ROWS);
        // Emitted by the render thread, right after the frame whose
        // rowsRebuilt it reads.
        connect(
            Window_.get(), &QQuickWindow::frameSwapped, this,
            [this] {
                ++Frames_;
                if (Item_->rowsRebuilt() > 0) {
                    ++DrawnFrames_;
                }
            },
            Qt::DirectConnection);
        Window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(Window_.get()));
        QVERIFY(QTest::qWaitForWindowActive(Window_.get()));
        Item_->forceActiveFocus();
        QVERIFY(QTest::qWaitFor([&] { return Item_->hasActiveFocus(); }));

        Controller_ = std::make_unique<SingleSession>(Item_, "claude", session);
        Ended_ = std::make_unique<QSignalSpy>(Controller_.get(),
                                              &SingleSession::Ended);
        Failed_ = std::make_unique<QSignalSpy>(Controller_.get(),
                                               &SingleSession::Failed);
        Controller_->Start(SocketPath());
    }

    // Waits until the TUI has had its queries answered: fake_tui reacts to a
    // key only if the key arrives in a read of its own.
    void Settle() {
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Controller_->Session()).contains(ANSWERED_ONCE), TIMEOUT_MS);
        // And until the frames of the output so far have been drawn.
        int seen = -1;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                QTest::qWait(200);
                const int now = DrawnFrames_;
                const bool still = now == seen;
                seen = now;
                return still;
            }(),
            TIMEOUT_MS);
    }

    // The window and the item, to a grid of the given size.
    void ResizeGrid(int columns, int rows) {
        const CellMetrics cell = MeasureCell(Item_->font());
        Window_->resize(columns * cell.Width, rows * cell.Height);
        Item_->setSize(QSizeF(Window_->size()));
        QCOMPARE(Item_->columns(), columns);
        QCOMPARE(Item_->rows(), rows);
    }

    // Runs the executable with the given leading arguments and --smoke, and
    // returns what it printed; the session mode shows itself by the line
    // that SingleSession logs on Opened.
    void RunExecutableInSessionMode(const QStringList& sessionArguments) {
        QProcessEnvironment environment = DaemonEnvironment();
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        environment.insert("QML_DISABLE_DISK_CACHE", "1");
        QProcess roost;
        roost.setProcessEnvironment(environment);
        roost.start(ROOST_PATH, sessionArguments + QStringList{"--smoke"});
        QVERIFY(roost.waitForFinished(TIMEOUT_MS * 3));
        QCOMPARE(roost.exitStatus(), QProcess::NormalExit);
        const QString errors = QString::fromUtf8(roost.readAllStandardError());
        const QString output = QString::fromUtf8(roost.readAllStandardOutput());
        QVERIFY2(roost.exitCode() == 0, qPrintable(errors));
        const QString opened =
            QStringLiteral("roost: session %1 of claude opened")
                .arg(SESSION_ID);
        QVERIFY2(errors.contains(opened) || output.contains(opened),
                 qPrintable(errors + output));
    }

    std::unique_ptr<QTemporaryDir> Dir_;
    ProcessLifeline Lifeline_;
    std::unique_ptr<QProcess> Daemon_;
    std::unique_ptr<QQuickWindow> Window_;
    TerminalItem* Item_ = nullptr;
    std::unique_ptr<SingleSession> Controller_;
    std::unique_ptr<QSignalSpy> Ended_;
    std::unique_ptr<QSignalSpy> Failed_;
    std::atomic<int> Frames_ = 0;
    std::atomic<int> DrawnFrames_ = 0;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
        Frames_ = 0;
        DrawnFrames_ = 0;
        QVERIFY(LaunchDaemon());
    }

    void cleanup() {
        // The controller before the window: it holds the item.
        Ended_.reset();
        Failed_.reset();
        Controller_.reset();
        Window_.reset();
        Item_ = nullptr;
        if (Daemon_ && Daemon_->state() != QProcess::NotRunning) {
            QCOMPARE(::kill(static_cast<pid_t>(Daemon_->processId()), SIGTERM),
                     0);
            QVERIFY(Daemon_->waitForFinished(TIMEOUT_MS));
            QCOMPARE(Daemon_->exitStatus(), QProcess::NormalExit);
            QCOMPARE(Daemon_->exitCode(), 0);
        }
        Daemon_.reset();
    }

    void TheTuiScreenIsShownAndDrawn() {
        ShowAndStart();
        QTRY_VERIFY_WITH_TIMEOUT(Controller_->Session() != nullptr, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Controller_->Session()).contains("fake tui cwd="),
            TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(DrawnFrames_ > 0, TIMEOUT_MS);
        QVERIFY(Failed_->isEmpty());
    }

    void TypedKeysReachTheTuiAndTheEchoIsRedrawn() {
        ShowAndStart();
        Settle();
        const int drawnBefore = DrawnFrames_;

        for (const char key : {'w', 'o', 'r', 'l', 'd'}) {
            QTest::keyClick(Window_.get(), key);
            // Each key in a read of its own: the split a slow runner makes.
            QTest::qWait(50);
        }

        // The TUI shows the bytes of its last read: keys sent one by one may
        // arrive in several reads, so the tail of "world" and the total of 5.
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Controller_->Session()).contains(TYPED_WORLD), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(DrawnFrames_ > drawnBefore, TIMEOUT_MS);
    }

    void AResizeOfTheItemReachesThePtyAndTheLocalTerminal() {
        ShowAndStart();
        Settle();
        QVERIFY(
            ProbeColumn(Controller_->Session()).endsWith("\x1b[1;80H\x1b[0m"));
        const int drawnBefore = DrawnFrames_;

        ResizeGrid(100, 30);

        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Controller_->Session()).contains(SizeLine(100, 30)),
            TIMEOUT_MS);
        QVERIFY(
            ProbeColumn(Controller_->Session()).endsWith("\x1b[1;100H\x1b[0m"));
        QTRY_VERIFY_WITH_TIMEOUT(DrawnFrames_ > drawnBefore, TIMEOUT_MS);
    }

    void ABurstOfSizesEndsWithTheLastOne() {
        ShowAndStart();
        Settle();

        // A window drag: many sizes, with the event loop running in between.
        for (int step = 0; step < 20; ++step) {
            ResizeGrid(60 + step, 20 + step % 7);
            QTest::qWait(1);
        }
        // The tail without the event loop: the first of these is in flight
        // while the others arrive, so only the latest may be sent after it.
        ResizeGrid(90, 28);
        ResizeGrid(95, 30);
        ResizeGrid(97, 31);

        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Controller_->Session()).contains(SizeLine(97, 31)),
            TIMEOUT_MS);
        QVERIFY(
            ProbeColumn(Controller_->Session()).endsWith("\x1b[1;97H\x1b[0m"));
        QVERIFY(Failed_->isEmpty());
    }

    void TheTuiExitingEndsTheController() {
        ShowAndStart();
        Settle();

        QTest::keyClick(Window_.get(), 'x');

        QTRY_COMPARE_WITH_TIMEOUT(Ended_->count(), 1, TIMEOUT_MS);
        QCOMPARE(Ended_->first().first().toString(),
                 QStringLiteral("exited with code 0"));
        QVERIFY(Failed_->isEmpty());
    }

    void TheExecutableOpensASessionAndExitsAfterTheFirstFrame() {
        RunExecutableInSessionMode({"--session", SESSION_ID});
    }

    // Qt takes the single dash for its own session-manager option.
    void TheExecutableTakesTheSingleDashSession() {
        RunExecutableInSessionMode({"-session", SESSION_ID});
    }

    void TheDaemonDyingFailsTheControllerOnce() {
        ShowAndStart();
        QTRY_VERIFY_WITH_TIMEOUT(Controller_->Session() != nullptr, TIMEOUT_MS);
        Settle();

        Daemon_->kill();
        QVERIFY(Daemon_->waitForFinished(TIMEOUT_MS));

        QTRY_COMPARE_WITH_TIMEOUT(Failed_->count(), 1, TIMEOUT_MS);
        QVERIFY(Failed_->first().first().toString().startsWith(
            "connection to roostd lost"));
        // Nothing more follows the first end.
        QTest::qWait(300);
        QCOMPARE(Failed_->count(), 1);
        QVERIFY(Ended_->isEmpty());
    }

    // roostd opens any id of a known agent, since the fake attach runs for
    // every id, so the failure comes from an agent that no plugin serves.
    void TheExecutableFailsOnAnUnknownSession() {
        QProcessEnvironment environment = DaemonEnvironment();
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        environment.insert("QML_DISABLE_DISK_CACHE", "1");
        QProcess roost;
        roost.setProcessEnvironment(environment);
        roost.start(ROOST_PATH,
                    {"--session", "nope", "--agent", "nope", "--smoke"});
        QVERIFY(roost.waitForFinished(TIMEOUT_MS * 3));
        QCOMPARE(roost.exitStatus(), QProcess::NormalExit);
        const QString errors = QString::fromUtf8(roost.readAllStandardError());
        QCOMPARE(roost.exitCode(), 1);
        QVERIFY2(errors.contains("cannot open session nope of nope"),
                 qPrintable(errors));
    }
};

QTEST_MAIN(SingleSessionTest)
#include "single_session_test.moc"
