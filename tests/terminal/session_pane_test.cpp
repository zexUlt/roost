// SessionPane against the real roostd with two fixture plugins, whose agents
// "fast-agent" and "slow-agent" attach to fake_tui (the slow one answers
// agent.attachCommand late): what the pane shows and which status it reports
// for an open, a quick switch, the end of the session, a refused open, a
// null, a burst of sizes and a lost connection.

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "helpers/roostd_process.hpp"
#include "terminal/io_thread.hpp"
#include "terminal/session_pane.hpp"

using namespace roost;
using roost::testing::RoostdProcess;

namespace {

constexpr int TIMEOUT_MS = 10000;
// How long the slow plugin holds back its answer to agent.attachCommand.
constexpr int SLOW_DELAY_MS = 300;
// How long to watch for something that must not happen.
constexpr int QUIET_MS = 500;
const QString FAST_AGENT = QStringLiteral("fast-agent");
const QString SLOW_AGENT = QStringLiteral("slow-agent");
const QString SESSION_ID = QStringLiteral("s1");
// Each query answered once; a digit after the 1 would be more than once.
const QRegularExpression ANSWERED_ONCE(
    QStringLiteral(R"(answers da1=1 cpr=1(?!\d))"));
const QRegularExpression ANSWERED_TWICE(
    QStringLiteral(R"(answers da1=2 cpr=2(?!\d))"));

SessionKey Fast() { return {FAST_AGENT, SESSION_ID}; }
SessionKey Slow() { return {SLOW_AGENT, SESSION_ID}; }

// What the terminal shows, as the VT sequences that rebuild it.
QString Screen(const std::shared_ptr<TerminalSession>& session) {
    if (!session) {
        return {};
    }
    const auto lock = session->Lock();
    return QString::fromUtf8(session->Dump(lock).value_or("dump failed"));
}

QString SizeLine(int columns, int rows) {
    return QStringLiteral("size %1x%2").arg(columns).arg(rows);
}

// The greeting fake_tui shows for the session of a plugin.
QString Greeting(const QString& plugin) {
    return QStringLiteral("greeting=%1-%2").arg(plugin, SESSION_ID);
}

}  // namespace

class SessionPaneTest : public QObject {
    Q_OBJECT

private:
    // The plugin's agent.json gets an answer to agent.attachCommand for its
    // session: fake_tui, with the plugin's name as its greeting.
    bool AddAttach(const QString& plugin, int delayMs) {
        QFile file(Daemon_->PluginsPath() + u'/' + plugin + "/agent.json");
        if (!file.open(QIODevice::ReadWrite)) {
            return false;
        }
        QJsonObject answers = QJsonDocument::fromJson(file.readAll()).object();
        QJsonObject attach{
            {"result",
             QJsonObject{{"program", FAKE_TUI_PATH},
                         // A short path: the greeting shares its row with the
                         // cwd, and must not wrap.
                         {"cwd", "/"},
                         {"env", QJsonObject{{"FAKE_TUI_GREETING",
                                              plugin + u'-' + SESSION_ID}}}}}};
        if (delayMs > 0) {
            attach.insert("delayMs", delayMs);
        }
        answers.insert("agent.attachCommand",
                       QJsonObject{{plugin + "-agent/" + SESSION_ID, attach}});
        file.resize(0);
        return file.write(QJsonDocument(answers).toJson()) > 0;
    }

    // The daemon with both plugins, the IO thread connected to it, and a pane
    // that records its status changes.
    bool Prepare() {
        if (!Daemon_->IsValid() ||
            !Daemon_->AddFixturePlugin("fast", FIXTURE_BACKEND_PATH) ||
            !Daemon_->AddFixturePlugin("slow", FIXTURE_BACKEND_PATH) ||
            !AddAttach("fast", 0) || !AddAttach("slow", SLOW_DELAY_MS) ||
            !Daemon_->Start(ROOSTD_PATH)) {
            return false;
        }
        Io_ = std::make_unique<IoThread>();
        if (!ConnectIo()) {
            return false;
        }
        Pane_ = std::make_unique<SessionPane>(*Io_);
        connect(Pane_.get(), &SessionPane::StatusChanged, Pane_.get(),
                [this] { Statuses_.push_back(Pane_->Status()); });
        return true;
    }

    bool ConnectIo() {
        QFuture<rpc::Result> hello = Io_->Connect(Daemon_->SocketPath());
        return QTest::qWaitFor([&] { return hello.isFinished(); },
                               TIMEOUT_MS) &&
               hello.result();
    }

    // Opens fast/s1 at the given grid and waits until it is Open and the TUI
    // has had its queries answered: fake_tui reacts to a key only if the key
    // arrives in a read of its own.
    void OpenFastAndSettle(quint16 columns = 80, quint16 rows = 24) {
        Pane_->SetGrid(columns, rows);
        Pane_->Show(Fast());
        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Open,
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Pane_->Session()).contains(ANSWERED_ONCE), TIMEOUT_MS);
    }

    std::unique_ptr<RoostdProcess> Daemon_;
    std::unique_ptr<IoThread> Io_;
    std::unique_ptr<SessionPane> Pane_;
    std::vector<EPaneStatus> Statuses_;
    // The channels that raised Updated, in order.
    std::vector<std::uint32_t> Channels_;

private slots:
    void init() {
        Daemon_ = std::make_unique<RoostdProcess>();
        Statuses_.clear();
        Channels_.clear();
    }

    void cleanup() {
        Pane_.reset();
        if (Io_) {
            Io_->Stop();
        }
        Io_.reset();
        Daemon_.reset();
    }

    void ShowOpensTheSessionAtTheGrid() {
        QVERIFY(Prepare());
        QCOMPARE(Pane_->Status(), EPaneStatus::None);
        Pane_->SetGrid(100, 30);

        Pane_->Show(Fast());

        QCOMPARE(Pane_->Status(), EPaneStatus::Opening);
        QVERIFY(!Pane_->Session());
        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Open,
                                  TIMEOUT_MS);
        QVERIFY(Pane_->Session());
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                const QString screen = Screen(Pane_->Session());
                return screen.contains("fake tui") &&
                       screen.contains(Greeting("fast")) &&
                       screen.contains(SizeLine(100, 30));
            }(),
            TIMEOUT_MS);
        const std::vector<EPaneStatus> expected{EPaneStatus::Opening,
                                                EPaneStatus::Open};
        QCOMPARE(Statuses_, expected);
    }

    // The answer of the slow open comes after the fast one: the pane must not
    // take it, and must close its channel.
    void AQuickSwitchEndsOnTheLastSession() {
        QVERIFY(Prepare());
        Pane_->SetGrid(80, 24);

        Pane_->Show(Slow());
        Pane_->Show(Fast());

        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Open,
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Pane_->Session()).contains(Greeting("fast")), TIMEOUT_MS);
        const auto session = Pane_->Session();
        // Until the slow answer has come and been dealt with.
        QTest::qWait(SLOW_DELAY_MS + QUIET_MS);
        QCOMPARE(Pane_->Status(), EPaneStatus::Open);
        QCOMPARE(Pane_->Session(), session);
        QVERIFY(Screen(Pane_->Session()).contains(Greeting("fast")));
        QVERIFY(!Screen(Pane_->Session()).contains(Greeting("slow")));
        QCOMPARE(Pane_->Key(), std::optional<SessionKey>(Fast()));
    }

    void TheSameSessionWhileOpenChangesNothing() {
        QVERIFY(Prepare());
        OpenFastAndSettle();
        QSignalSpy sessionChanged(Pane_.get(), &SessionPane::SessionChanged);
        const auto statusChanges = Statuses_.size();
        const auto session = Pane_->Session();

        Pane_->Show(Fast());

        QCOMPARE(Pane_->Status(), EPaneStatus::Open);
        QCOMPARE(Pane_->Session(), session);
        QCOMPARE(sessionChanged.count(), 0);
        QCOMPARE(Statuses_.size(), statusChanges);
    }

    void AnEndedSessionKeepsItsScreenAndOpensAgain() {
        QVERIFY(Prepare());
        OpenFastAndSettle();
        const auto session = Pane_->Session();

        Pane_->SendInput("x");

        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Ended,
                                  TIMEOUT_MS);
        QCOMPARE(Pane_->Message(), QStringLiteral("exited with code 0"));
        QCOMPARE(Pane_->Session(), session);
        const QString last = Screen(session);
        QVERIFY(last.contains("fake tui"));
        // Input after the end goes nowhere.
        Pane_->SendInput("q");
        QTest::qWait(100);
        QCOMPARE(Screen(session), last);
        QCOMPARE(Pane_->Status(), EPaneStatus::Ended);

        // The same session again opens it anew.
        Pane_->Show(Fast());

        QCOMPARE(Pane_->Status(), EPaneStatus::Opening);
        QVERIFY(!Pane_->Session());
        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Open,
                                  TIMEOUT_MS);
        QVERIFY(Pane_->Session() != session);
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Pane_->Session()).contains(Greeting("fast")), TIMEOUT_MS);
        QVERIFY(Pane_->Message().isEmpty());
    }

    void ReopenOpensAnEndedSessionAndNothingElse() {
        QVERIFY(Prepare());
        OpenFastAndSettle();
        const auto session = Pane_->Session();
        Pane_->Reopen();
        QCOMPARE(Pane_->Status(), EPaneStatus::Open);
        QCOMPARE(Pane_->Session(), session);

        Pane_->SendInput("x");
        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Ended,
                                  TIMEOUT_MS);
        Pane_->Reopen();

        QCOMPARE(Pane_->Status(), EPaneStatus::Opening);
        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Open,
                                  TIMEOUT_MS);
    }

    void AnUnknownAgentFailsWithTheMessageOfRoostd() {
        QVERIFY(Prepare());
        // What roostd says to the same request.
        QFuture<OpenResult> direct = Io_->Open("nope", SESSION_ID, 80, 24);
        QVERIFY(
            QTest::qWaitFor([&] { return direct.isFinished(); }, TIMEOUT_MS));
        QVERIFY(!direct.result());
        const QString expected = direct.result().error().Message;
        QVERIFY(!expected.isEmpty());

        Pane_->Show(SessionKey{"nope", SESSION_ID});

        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Failed,
                                  TIMEOUT_MS);
        QCOMPARE(Pane_->Message(), expected);
        QVERIFY(!Pane_->Session());
        // The same session again tries again.
        Pane_->Show(SessionKey{"nope", SESSION_ID});
        QCOMPARE(Pane_->Status(), EPaneStatus::Opening);
        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Failed,
                                  TIMEOUT_MS);
    }

    void NullClosesTheChannelAndClearsTheScreen() {
        QVERIFY(Prepare());
        // The channel of the session, learned from its output.
        connect(
            Io_.get(), &IoThread::Updated, Io_.get(),
            [this](std::uint32_t channel) { Channels_.push_back(channel); });
        OpenFastAndSettle();
        Pane_->SendInput("q");
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Pane_->Session()).contains(ANSWERED_TWICE), TIMEOUT_MS);
        QVERIFY(!Channels_.empty());
        const std::uint32_t channel = Channels_.back();
        const auto old = Pane_->Session();
        QSignalSpy sessionChanged(Pane_.get(), &SessionPane::SessionChanged);

        Pane_->Show(std::nullopt);

        QVERIFY(!Pane_->Session());
        QCOMPARE(Pane_->Status(), EPaneStatus::None);
        QVERIFY(!Pane_->Key());
        QCOMPARE(sessionChanged.count(), 1);
        // Input goes nowhere, and the channel is closed: a key sent to it
        // directly is dropped, so the old terminal gets no third answer.
        Pane_->SendInput("q");
        Io_->SendInput(channel, "q");
        QTest::qWait(QUIET_MS);
        QVERIFY(!Screen(old).contains(QRegularExpression("da1=3")));
        QVERIFY(Screen(old).contains(ANSWERED_TWICE));
        // And a new session can be shown.
        Pane_->Show(Fast());
        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Open,
                                  TIMEOUT_MS);
    }

    void ABurstOfSizesEndsWithTheLastOne() {
        QVERIFY(Prepare());
        OpenFastAndSettle();

        // A window drag: many sizes, with the event loop running in between.
        for (int step = 0; step < 20; ++step) {
            Pane_->SetGrid(static_cast<quint16>(60 + step),
                           static_cast<quint16>(20 + step % 7));
            QTest::qWait(1);
        }
        // The tail without the event loop: the first of these is in flight
        // while the others arrive, so only the latest may be sent after it.
        Pane_->SetGrid(90, 28);
        Pane_->SetGrid(95, 30);
        Pane_->SetGrid(97, 31);

        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Pane_->Session()).contains(SizeLine(97, 31)), TIMEOUT_MS);
        QCOMPARE(Pane_->Status(), EPaneStatus::Open);
    }

    void ALostConnectionKeepsTheScreenAndReopensWhenBack() {
        QVERIFY(Prepare());
        OpenFastAndSettle();
        const auto session = Pane_->Session();

        QVERIFY(Daemon_->Stop());

        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Disconnected,
                                  TIMEOUT_MS);
        QVERIFY(!Pane_->Message().isEmpty());
        QCOMPARE(Pane_->Session(), session);
        QVERIFY(Screen(session).contains("fake tui"));
        // Input goes nowhere.
        Pane_->SendInput("q");

        // The window's Api connects again; the pane opens the session anew.
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(ConnectIo());
        Pane_->Reopen();

        QCOMPARE(Pane_->Status(), EPaneStatus::Opening);
        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Open,
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Pane_->Session()).contains(Greeting("fast")), TIMEOUT_MS);
    }

    // The late channel must be closed and nothing of the pane touched.
    void APaneGoneWhileOpeningLeavesNoCrash() {
        QVERIFY(Prepare());
        Pane_->Show(Slow());
        QCOMPARE(Pane_->Status(), EPaneStatus::Opening);

        Pane_.reset();
        QTest::qWait(SLOW_DELAY_MS + QUIET_MS);

        // The IO thread still serves a new pane.
        Pane_ = std::make_unique<SessionPane>(*Io_);
        Pane_->Show(Fast());
        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Open,
                                  TIMEOUT_MS);
    }
};

QTEST_MAIN(SessionPaneTest)
#include "session_pane_test.moc"
