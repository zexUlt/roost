#include <QDir>
#include <QEventLoop>
#include <QList>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <memory>
#include <utility>

#include "core/pty.hpp"
#include "helpers/scoped_env.hpp"

using namespace roost;
using namespace std::chrono_literals;

namespace {

constexpr int TIMEOUT_MS = 5000;

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ROOST_UNDER_ASAN 1
#endif
#endif
#if defined(ROOST_UNDER_ASAN)
constexpr bool UNDER_ASAN = true;
#else
constexpr bool UNDER_ASAN = false;
#endif

PtySpec FakeTui(QStringList arguments = {}) {
    return PtySpec{.Program = FAKE_TUI_PATH,
                   .Arguments = std::move(arguments),
                   .WorkingDirectory = {},
                   .Environment = {}};
}

// A shell that ignores the hang-up, with a child that ignores it too and
// prints "child=<pid>".
PtySpec Grandchild() {
    return PtySpec{
        .Program = "/bin/sh",
        .Arguments = {"-c", "trap '' HUP; sleep 77 & echo child=$!; wait"},
        .WorkingDirectory = {},
        .Environment = {}};
}

// Once nobody is left to reap it, launchd or init does.
bool Gone(pid_t pid) {
    return QTest::qWaitFor(
        [pid] { return ::kill(pid, 0) == -1 && errno == ESRCH; }, TIMEOUT_MS);
}

QList<int> OpenDescriptors() {
    QList<int> open;
    for (int fd = 0; fd < 1024; ++fd) {
        if (::fcntl(fd, F_GETFD) != -1) {
            open.append(fd);
        }
    }
    return open;
}

// Everything the PTY has printed, collected as it comes.
class Screen {
public:
    explicit Screen(Pty& pty) : Pty_(pty) {
        QObject::connect(&pty, &Pty::Output, &pty,
                         [this](const QByteArray& bytes) { Printed += bytes; });
    }

    // Runs the event loop rather than polling it: qWaitFor sleeps between
    // turns, which throttles a program that waits for its input to be taken.
    bool WaitFor(const QByteArray& text) const {
        if (Printed.contains(text)) {
            return true;
        }
        QEventLoop loop;
        QTimer::singleShot(TIMEOUT_MS, &loop, &QEventLoop::quit);
        // After the connection that collects, so Printed is up to date.
        QObject::connect(&Pty_, &Pty::Output, &loop, [&] {
            if (Printed.contains(text)) {
                loop.quit();
            }
        });
        loop.exec();
        return Printed.contains(text);
    }

    QByteArray Printed;

private:
    Pty& Pty_;
};

pid_t ChildOf(const Screen& screen) {
    if (!screen.WaitFor("\n")) {
        return -1;
    }
    const auto match = QRegularExpression("child=(\\d+)")
                           .match(QString::fromLatin1(screen.Printed));
    return match.hasMatch() ? static_cast<pid_t>(match.captured(1).toInt())
                            : -1;
}

}  // namespace

class PtyTest : public QObject {
    Q_OBJECT

private slots:
    void RunsInATerminalOfItsOwn() {
        // Not inherited from whatever terminal runs the test.
        const testing::ScopedEnv term("TERM", "dumb");
        Pty pty;
        const Screen screen(pty);
        QVERIFY(pty.Start(FakeTui(), {}));
        QVERIFY2(screen.WaitFor("tty yes term=xterm-256color"),
                 screen.Printed.constData());
        const auto pid = static_cast<pid_t>(pty.ProcessId());
        QCOMPARE(::getsid(pid), pid);
        QCOMPARE(::getpgid(pid), pid);
    }

    void GetsTheEnvironmentAndDirectoryAsked() {
        const QTemporaryDir dir;
        PtySpec spec = FakeTui();
        spec.WorkingDirectory = dir.path();
        spec.Environment = {"FAKE_TUI_GREETING=hi", "TERM=vt100", "junk"};
        Pty pty;
        const Screen screen(pty);
        QVERIFY(pty.Start(spec, {}));
        // macOS reports /private/var for /var.
        const QByteArray cwd = QDir(dir.path()).canonicalPath().toUtf8();
        QVERIFY2(screen.WaitFor("fake tui cwd=" + cwd + " greeting=hi"),
                 screen.Printed.constData());
        QVERIFY2(screen.WaitFor("term=vt100"), screen.Printed.constData());
    }

    void StartsAtTheSizeGiven() {
        Pty pty;
        const Screen screen(pty);
        QVERIFY(pty.Start(FakeTui(), {.Columns = 100, .Rows = 30}));
        QVERIFY2(screen.WaitFor("size 100x30"), screen.Printed.constData());
        QCOMPARE(pty.Size(), (PtySize{.Columns = 100, .Rows = 30}));
    }

    void ResizeReachesTheProgram() {
        Pty pty;
        const Screen screen(pty);
        QVERIFY(pty.Start(FakeTui(), {}));
        QVERIFY(screen.WaitFor("size 80x24"));
        QVERIFY(pty.Resize({.Columns = 120, .Rows = 40}));
        QVERIFY2(screen.WaitFor("size 120x40"), screen.Printed.constData());
        QCOMPARE(pty.Size(), (PtySize{.Columns = 120, .Rows = 40}));
    }

    void InputReachesTheProgram() {
        Pty pty;
        const Screen screen(pty);
        QVERIFY(pty.Start(FakeTui(), {}));
        QVERIFY(screen.WaitFor("answers da1=0"));
        pty.Write("hi");
        QVERIFY2(screen.WaitFor("input hi total=2"),
                 screen.Printed.constData());
    }

    void LargeInputIsWrittenWhole() {
        Pty pty;
        const Screen screen(pty);
        QVERIFY(pty.Start(FakeTui(), {}));
        QVERIFY(screen.WaitFor("answers da1=0"));
        // Far more than the terminal buffers: the rest waits for the reader.
        // No 'e', 'q' or 'x', which fake_tui takes for keys when read alone.
        pty.Write(QByteArray(1024 * 1024, 'a') + "zzz");
        QVERIFY2(screen.WaitFor(" total=1048579"),
                 screen.Printed.right(200).constData());
    }

    void AnswersArriveAsInput() {
        Pty pty;
        const Screen screen(pty);
        QVERIFY(pty.Start(FakeTui(), {}));
        QVERIFY(screen.WaitFor("\x1b[6n"));
        // As a terminal emulator would answer DA1 and CPR.
        pty.Write("\x1b[?62;22c\x1b[3;1R");
        QVERIFY2(screen.WaitFor("answers da1=1 cpr=1"),
                 screen.Printed.constData());
    }

    void OutputBeforeExitIsNotLost() {
        Pty pty;
        const Screen screen(pty);
        QByteArray printedAtFinish;
        QString reason;
        connect(&pty, &Pty::Finished, this, [&](const QString& why) {
            printedAtFinish = screen.Printed;
            reason = why;
        });
        QVERIFY(pty.Start(FakeTui(), {}));
        QVERIFY(screen.WaitFor("answers da1=0"));
        pty.Write("x");
        // The exit and the last output then wait for the same turn of the
        // event loop, in whichever order it takes them.
        QTest::qSleep(200);
        QTRY_VERIFY_WITH_TIMEOUT(!reason.isEmpty(), TIMEOUT_MS);
        QCOMPARE(reason, "exited with code 0");
        QVERIFY2(printedAtFinish.contains("bye"), printedAtFinish.constData());
        QVERIFY(!pty.IsRunning());
    }

    void ExitCodeIsReported() {
        Pty pty;
        QSignalSpy finished(&pty, &Pty::Finished);
        QVERIFY(pty.Start(FakeTui(), {}));
        pty.Write("e");
        QVERIFY(finished.wait(TIMEOUT_MS));
        QCOMPARE(finished.takeFirst().at(0).toString(), "exited with code 7");
        // Closed with the program.
        pty.Write("ignored");
        QVERIFY(!pty.Resize({.Columns = 10, .Rows = 10}));
    }

    void HangUpEndsTheProgram() {
        // The kill comes long after the wait below gives up.
        Pty pty(PtyOptions{.HangUpTimeout = 60s});
        const Screen screen(pty);
        QSignalSpy finished(&pty, &Pty::Finished);
        QVERIFY(pty.Start(FakeTui(), {}));
        QVERIFY(screen.WaitFor("answers da1=0"));
        pty.HangUp();
        QVERIFY(finished.wait(TIMEOUT_MS));
        QCOMPARE(finished.count(), 1);
    }

    void OneThatIgnoresTheHangUpIsKilled() {
        Pty pty(PtyOptions{.HangUpTimeout = 200ms});
        const Screen screen(pty);
        QSignalSpy finished(&pty, &Pty::Finished);
        QVERIFY(pty.Start(FakeTui({"--ignore-hangup"}), {}));
        QVERIFY(screen.WaitFor("answers da1=0"));
        pty.HangUp();
        QVERIFY(finished.wait(TIMEOUT_MS));
        QCOMPARE(finished.takeFirst().at(0).toString(),
                 "was killed by a signal");
    }

    void WhatItStartedIsKilledWithIt() {
        Pty pty(PtyOptions{.HangUpTimeout = 200ms});
        const Screen screen(pty);
        QSignalSpy finished(&pty, &Pty::Finished);
        QVERIFY(pty.Start(Grandchild(), {}));
        const pid_t child = ChildOf(screen);
        QVERIFY(child > 0);
        pty.HangUp();
        QVERIFY(finished.wait(TIMEOUT_MS));
        QVERIFY2(Gone(child), "the child that ignored the hang-up lives on");
    }

    void StartsWithStdioClosed() {
        // As roostd may run. openpty then hands out descriptors 0 to 2, and Qt
        // points the child's 0 to 2 at /dev/null.
        const int savedIn = ::dup(0);
        const int savedErr = ::dup(2);
        ::close(0);
        ::close(2);
        Pty pty;
        const Screen screen(pty);
        const auto started = pty.Start(FakeTui(), {});
        ::dup2(savedIn, 0);
        ::dup2(savedErr, 2);
        ::close(savedIn);
        ::close(savedErr);
        QVERIFY2(started, qPrintable(started ? QString() : started.error()));
        QVERIFY2(screen.WaitFor("tty yes"), screen.Printed.constData());
    }

    void OutputHoldsOnlyItsBytes() {
        // Listeners may keep what they get.
        Pty pty;
        qsizetype waste = 0;
        connect(&pty, &Pty::Output, this, [&waste](const QByteArray& bytes) {
            waste = std::max(waste, bytes.capacity() - bytes.size());
        });
        const Screen screen(pty);
        QVERIFY(pty.Start(FakeTui(), {}));
        QVERIFY(screen.WaitFor("answers da1=0"));
        QCOMPARE_LT(waste, 1024);
    }

    void AMissingProgramFailsToStart() {
        Pty pty;
        const auto started =
            pty.Start(PtySpec{.Program = "/nonexistent/program",
                              .Arguments = {},
                              .WorkingDirectory = {},
                              .Environment = {}},
                      {});
        QVERIFY(!started);
        QVERIFY2(!started.error().isEmpty(), qPrintable(started.error()));
        QVERIFY(!pty.IsRunning());
    }

    void StartsOnlyOnce() {
        Pty pty;
        QVERIFY(pty.Start(FakeTui(), {}));
        const auto again = pty.Start(FakeTui(), {});
        QVERIFY(!again);
        QCOMPARE(again.error(), "the PTY is already in use");
    }

    void DestroyingItKillsTheProgram() {
        QTest::failOnWarning(QRegularExpression("Destroyed while process"));
        auto pty = std::make_unique<Pty>();
        const Screen screen(*pty);
        QVERIFY(pty->Start(FakeTui({"--ignore-hangup"}), {}));
        QVERIFY(screen.WaitFor("answers da1=0"));
        const auto pid = static_cast<pid_t>(pty->ProcessId());
        QSignalSpy finished(pty.get(), &Pty::Finished);
        pty.reset();
        QCOMPARE(finished.count(), 0);
        // Reaped as well as killed.
        QCOMPARE(::kill(pid, 0), -1);
        QCOMPARE(errno, ESRCH);
    }

    void DestroyingItKillsWhatTheProgramStarted() {
        auto pty = std::make_unique<Pty>();
        const Screen screen(*pty);
        QVERIFY(pty->Start(Grandchild(), {}));
        const pid_t child = ChildOf(screen);
        QVERIFY(child > 0);
        pty.reset();
        QVERIFY2(Gone(child), "the child of the program lives on");
    }

    void LeavesNoDescriptorsBehind() {
        const QList<int> before = OpenDescriptors();
        {
            Pty pty;
            QSignalSpy finished(&pty, &Pty::Finished);
            QVERIFY(pty.Start(FakeTui(), {}));
            pty.Write("e");
            QVERIFY(finished.wait(TIMEOUT_MS));
        }
        {
            Pty pty;
            QVERIFY(!pty.Start(PtySpec{.Program = "/nonexistent/program",
                                       .Arguments = {},
                                       .WorkingDirectory = {},
                                       .Environment = {}},
                               {}));
        }
        QCOMPARE(OpenDescriptors(), before);
    }

    void InheritsNoDescriptors() {
        if (UNDER_ASAN) {
            QSKIP("AddressSanitizer keeps descriptors of its own open");
        }
        Pty pty;
        const Screen screen(pty);
        QVERIFY(pty.Start(FakeTui(), {}));
        QVERIFY2(screen.WaitFor(" fds=\x1b"), screen.Printed.constData());
    }
};

QTEST_GUILESS_MAIN(PtyTest)
#include "pty_test.moc"
