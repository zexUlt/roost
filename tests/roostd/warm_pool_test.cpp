#include <QEventLoop>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <cerrno>
#include <csignal>
#include <expected>
#include <utility>

#include "core/vt_terminal.hpp"
#include "roostd/warm_pool.hpp"

using namespace roost;
using namespace std::chrono_literals;

namespace {

constexpr int TIMEOUT_MS = 5000;

PtySpec FakeTui(QStringList arguments = {}) {
    return PtySpec{.Program = FAKE_TUI_PATH,
                   .Arguments = std::move(arguments),
                   .WorkingDirectory = {},
                   .Environment = {}};
}

PtySpec Shell(const QString& script) {
    return PtySpec{.Program = "/bin/sh",
                   .Arguments = {"-c", script},
                   .WorkingDirectory = {},
                   .Environment = {}};
}

WarmSessionKey Key(const QString& session) {
    return WarmSessionKey{.Agent = "fake", .Session = session};
}

// The error of a result that failed; QVERIFY2 evaluates its message anyway.
template <typename TValue>
QString ErrorOf(const std::expected<TValue, QString>& result) {
    return result ? QString() : result.error();
}

// Once nobody is left to reap it, launchd or init does.
bool Gone(qint64 pid) {
    return QTest::qWaitFor(
        [pid] {
            return ::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH;
        },
        TIMEOUT_MS);
}

// Everything a session has emitted, collected as it comes.
class Screen {
public:
    explicit Screen(WarmSession* session) : Session_(session) {
        QObject::connect(session, &WarmSession::Output, session,
                         [this](const QByteArray& bytes) { Printed += bytes; });
    }

    // Runs the event loop rather than polling it, as in pty_test.
    bool WaitFor(const QByteArray& text) const {
        if (Printed.contains(text)) {
            return true;
        }
        QEventLoop loop;
        QTimer::singleShot(TIMEOUT_MS, &loop, &QEventLoop::quit);
        QObject::connect(Session_, &WarmSession::Output, &loop, [&] {
            if (Printed.contains(text)) {
                loop.quit();
            }
        });
        loop.exec();
        return Printed.contains(text);
    }

    QByteArray Printed;

private:
    WarmSession* Session_;
};

// The screen of the session's terminal, as a window would restore it.
QByteArray DumpOf(const WarmSession& session) {
    const auto snapshot = session.Snapshot();
    if (!snapshot) {
        return "snapshot: " + snapshot.error().toUtf8();
    }
    const auto terminal = VtTerminal::FromSnapshot(*snapshot);
    if (!terminal) {
        return "restore: " + terminal.error().toUtf8();
    }
    return terminal->Dump().value_or("dump failed");
}

}  // namespace

class WarmPoolTest : public QObject {
    Q_OBJECT

private slots:
    void TheProgramGetsOneAnswerToEachQuery() {
        WarmPool pool;
        auto session = pool.Open(Key("a"), FakeTui(), {});
        QVERIFY2(session, qPrintable(ErrorOf(session)));
        const Screen screen(*session);
        QVERIFY2(screen.WaitFor("answers da1=1 cpr=1"),
                 screen.Printed.constData());
        (*session)->Write("q");
        QVERIFY2(screen.WaitFor("answers da1=2 cpr=2"),
                 screen.Printed.constData());
        // Input after the answers: once it is drawn, any extra answer would
        // have been counted.
        (*session)->Write("z");
        QVERIFY2(screen.WaitFor("input z total=1"), screen.Printed.constData());
        const QByteArray dump = DumpOf(**session);
        QVERIFY2(dump.contains("answers da1=2 cpr=2"), dump.constData());
    }

    void ASnapshotAndTheOutputAfterItMakeTheSameScreen() {
        WarmPool pool;
        auto session = pool.Open(Key("a"), FakeTui(), {});
        QVERIFY2(session, qPrintable(ErrorOf(session)));
        Screen screen(*session);
        QVERIFY(screen.WaitFor("answers da1=1 cpr=1"));
        // The window's copy: restored from a snapshot, fed what comes next,
        // and silent, or the program would count two answers to a query.
        const auto snapshot = (*session)->Snapshot();
        QVERIFY2(snapshot, qPrintable(ErrorOf(snapshot)));
        auto window = VtTerminal::FromSnapshot(*snapshot);
        QVERIFY2(window, qPrintable(ErrorOf(window)));
        connect(*session, &WarmSession::Output, *session,
                [&](const QByteArray& bytes) { window->Write(bytes); });
        (*session)->Write("q");
        QVERIFY(screen.WaitFor("answers da1=2 cpr=2"));
        (*session)->Write("hello");
        QVERIFY(screen.WaitFor("input hello total=5"));
        QCOMPARE(window->Dump().value_or("window"), DumpOf(**session));
    }

    void OpeningAWarmSessionGivesItBack() {
        WarmPool pool;
        auto first = pool.Open(Key("a"), FakeTui(), {});
        QVERIFY2(first, qPrintable(ErrorOf(first)));
        auto second = pool.Open(Key("a"), FakeTui(), {.Columns = 100});
        QVERIFY2(second, qPrintable(ErrorOf(second)));
        QCOMPARE(*second, *first);
        QCOMPARE(pool.Count(), 1);
        QCOMPARE((*first)->Size(), PtySize{});
        QCOMPARE(pool.Find(Key("a")), *first);
        QCOMPARE(pool.Find(Key("b")), nullptr);
        QCOMPARE(pool.Find(WarmSessionKey{.Agent = "other", .Session = "a"}),
                 nullptr);
    }

    void TheLeastRecentlyViewedIsEvicted() {
        WarmPool pool({.Capacity = 2, .Pty = {}});
        auto a = pool.Open(Key("a"), FakeTui(), {});
        auto b = pool.Open(Key("b"), FakeTui(), {});
        QVERIFY(a && b);
        const qint64 bPid = (*b)->ProcessId();
        QSignalSpy aClosed(*a, &WarmSession::Closed);
        QSignalSpy bClosed(*b, &WarmSession::Closed);
        const QPointer<WarmSession> evicted = *b;
        pool.Touch(*a);
        auto c = pool.Open(Key("c"), FakeTui(), {});
        QVERIFY2(c, qPrintable(ErrorOf(c)));
        QCOMPARE(bClosed.size(), 1);
        QCOMPARE(bClosed.first().first().toString(), "evicted");
        QVERIFY(evicted->IsClosed());
        QCOMPARE(pool.Find(Key("b")), nullptr);
        QCOMPARE(pool.Count(), 2);
        QCOMPARE(pool.Find(Key("a")), *a);
        QVERIFY(aClosed.isEmpty());
        // Hung up, gone, and deleted once it has.
        QVERIFY(Gone(bPid));
        QVERIFY(QTest::qWaitFor([&] { return evicted.isNull(); }, TIMEOUT_MS));
        QCOMPARE(bClosed.size(), 1);

        // Opening makes a session viewed too: c is newer than a.
        auto d = pool.Open(Key("d"), FakeTui(), {});
        QVERIFY(d);
        QCOMPARE(aClosed.size(), 1);
        QCOMPARE(pool.Find(Key("c")), *c);
    }

    void OpeningAWarmSessionCountsAsViewing() {
        WarmPool pool({.Capacity = 2, .Pty = {}});
        auto a = pool.Open(Key("a"), FakeTui(), {});
        auto b = pool.Open(Key("b"), FakeTui(), {});
        QVERIFY(a && b);
        QSignalSpy aClosed(*a, &WarmSession::Closed);
        QSignalSpy bClosed(*b, &WarmSession::Closed);
        auto again = pool.Open(Key("a"), FakeTui(), {});
        QVERIFY2(again, qPrintable(ErrorOf(again)));
        QCOMPARE(*again, *a);
        auto c = pool.Open(Key("c"), FakeTui(), {});
        QVERIFY2(c, qPrintable(ErrorOf(c)));
        QCOMPARE(bClosed.size(), 1);
        QCOMPARE(bClosed.first().first().toString(), "evicted");
        QVERIFY(aClosed.isEmpty());
        QCOMPARE(pool.Find(Key("a")), *a);
        QCOMPARE(pool.Find(Key("c")), *c);
    }

    void AnEvictedProgramThatIgnoresTheHangUpIsKilled() {
        WarmPool pool({.Capacity = 1, .Pty = {.HangUpTimeout = 200ms}});
        auto stubborn = pool.Open(Key("a"), FakeTui({"--ignore-hangup"}), {});
        QVERIFY(stubborn);
        const Screen screen(*stubborn);
        // Its SIGHUP handler is in place once it draws.
        QVERIFY(screen.WaitFor("answers"));
        const qint64 pid = (*stubborn)->ProcessId();
        const QPointer<WarmSession> evicted = *stubborn;
        QVERIFY(pool.Open(Key("b"), FakeTui(), {}));
        QVERIFY(Gone(pid));
        QVERIFY(QTest::qWaitFor([&] { return evicted.isNull(); }, TIMEOUT_MS));
    }

    void AKeyOpenedAgainWhileItsEvictedProgramHangsUpStaysWarm() {
        WarmPool pool({.Capacity = 1, .Pty = {.HangUpTimeout = 200ms}});
        auto old = pool.Open(Key("a"), FakeTui({"--ignore-hangup"}), {});
        QVERIFY(old);
        const Screen screen(*old);
        QVERIFY(screen.WaitFor("answers"));
        const QPointer<WarmSession> evicted = *old;
        QVERIFY(pool.Open(Key("b"), FakeTui(), {}));
        auto again = pool.Open(Key("a"), FakeTui(), {});
        QVERIFY(again);
        QVERIFY(*again != evicted.data());
        QVERIFY(QTest::qWaitFor([&] { return evicted.isNull(); }, TIMEOUT_MS));
        QCOMPARE(pool.Find(Key("a")), *again);
        QVERIFY(!(*again)->IsClosed());
    }

    void AnExitClosesTheSession() {
        WarmPool pool;
        auto session = pool.Open(Key("a"), FakeTui(), {});
        QVERIFY(session);
        const Screen screen(*session);
        // The key goes in a read of its own only after the answers.
        QVERIFY(screen.WaitFor("answers da1=1 cpr=1"));
        QSignalSpy closed(*session, &WarmSession::Closed);
        const QPointer<WarmSession> exited = *session;
        (*session)->Write("e");
        QVERIFY(closed.wait(TIMEOUT_MS));
        QCOMPARE(closed.first().first().toString(), "exited with code 7");
        QCOMPARE(pool.Find(Key("a")), nullptr);
        QCOMPARE(pool.Count(), 0);
        QVERIFY(QTest::qWaitFor([&] { return exited.isNull(); }, TIMEOUT_MS));
        QCOMPARE(closed.size(), 1);
    }

    void ResizeReachesTheProgramAndTheTerminal() {
        WarmPool pool;
        // The cursor report comes from the daemon's terminal, and the PTY
        // echoes it: the column tells the terminal's width.
        auto session = pool.Open(
            Key("a"),
            Shell("read x; stty size; printf '%0100d\\033[6n' 0; read y"), {});
        QVERIFY2(session, qPrintable(ErrorOf(session)));
        const Screen screen(*session);
        QVERIFY((*session)->Resize({.Columns = 120, .Rows = 30}));
        QCOMPARE((*session)->Size(), (PtySize{.Columns = 120, .Rows = 30}));
        (*session)->Write("go\n");
        QVERIFY2(screen.WaitFor("30 120"), screen.Printed.constData());
        QVERIFY2(screen.WaitFor(";101R"), screen.Printed.constData());
    }

    void ABadSizeIsRefused() {
        WarmPool pool;
        auto refused =
            pool.Open(Key("a"), FakeTui(), {.Columns = 0, .Rows = 24});
        QVERIFY(!refused);
        QCOMPARE(pool.Count(), 0);
        auto session = pool.Open(Key("a"), FakeTui(), {});
        QVERIFY(session);
        QVERIFY(!(*session)->Resize({.Columns = 2001, .Rows = 24}));
        QVERIFY(!(*session)->Resize({.Columns = 80, .Rows = 0}));
        QCOMPARE((*session)->Size(), PtySize{});
    }

    void AProgramThatFailsToStartEvictsNothing() {
        WarmPool pool({.Capacity = 1, .Pty = {}});
        auto a = pool.Open(Key("a"), FakeTui(), {});
        QVERIFY(a);
        QSignalSpy closed(*a, &WarmSession::Closed);
        auto broken = pool.Open(Key("b"),
                                PtySpec{.Program = "/nonexistent/roost-agent",
                                        .Arguments = {},
                                        .WorkingDirectory = {},
                                        .Environment = {}},
                                {});
        QVERIFY(!broken);
        QCOMPARE(pool.Count(), 1);
        QCOMPARE(pool.Find(Key("b")), nullptr);
        QVERIFY(closed.isEmpty());
        QVERIFY(!(*a)->IsClosed());
    }

    void AClosedSessionTakesNoInputOrResize() {
        WarmPool pool({.Capacity = 1, .Pty = {}});
        auto a = pool.Open(Key("a"), FakeTui(), {});
        QVERIFY(a);
        const QPointer<WarmSession> evicted = *a;
        QVERIFY(pool.Open(Key("b"), FakeTui(), {}));
        QVERIFY(evicted->IsClosed());
        QVERIFY(!evicted->Resize({.Columns = 100, .Rows = 30}));
        evicted->Write("x");
    }

    void DestroyingThePoolKillsItsPrograms() {
        qint64 pid = 0;
        bool closed = false;
        {
            WarmPool pool;
            auto session =
                pool.Open(Key("a"), FakeTui({"--ignore-hangup"}), {});
            QVERIFY(session);
            pid = (*session)->ProcessId();
            connect(*session, &WarmSession::Closed, this,
                    [&closed] { closed = true; });
        }
        QVERIFY(Gone(pid));
        QVERIFY(!closed);
    }
};

QTEST_GUILESS_MAIN(WarmPoolTest)
#include "warm_pool_test.moc"
