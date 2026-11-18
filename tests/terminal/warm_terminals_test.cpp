// WarmTerminals against the real roostd with two fixture plugins, whose agents
// "fast-agent" (sessions s1 to s5) and "slow-agent" (s1) attach to fake_tui,
// the slow one answering agent.attachCommand late: what stays warm when a pane
// switches, what the window tells roostd, and what it drops when roostd closes
// a channel or the connection is lost. The panes in these tests are
// SessionPane, the view the window uses.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTest>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "helpers/channels_left.hpp"
#include "helpers/counting_proxy.hpp"
#include "helpers/roostd_process.hpp"
#include "terminal/io_thread.hpp"
#include "terminal/session_pane.hpp"
#include "terminal/warm_terminals.hpp"

using namespace roost;
using roost::testing::ChannelsLeft;
using roost::testing::CountingProxy;
using roost::testing::RoostdProcess;

namespace {

constexpr int TIMEOUT_MS = 10000;
// How long the slow plugin holds back its answer to agent.attachCommand.
constexpr int SLOW_DELAY_MS = 300;
// roostd's pool holds this many sessions (WarmPoolOptions::Capacity).
constexpr int DAEMON_CAPACITY = 4;
constexpr int FAST_SESSIONS = DAEMON_CAPACITY + 1;
const QString FAST_AGENT = QStringLiteral("fast-agent");
const QString SLOW_AGENT = QStringLiteral("slow-agent");
// Each query answered once; a digit after the 1 would be more than once.
const QRegularExpression ANSWERED_ONCE(
    QStringLiteral(R"(answers da1=1 cpr=1(?!\d))"));
const QRegularExpression ANSWERED_TWICE(
    QStringLiteral(R"(answers da1=2 cpr=2(?!\d))"));
const QRegularExpression ANSWERED_FOUR_TIMES(
    QStringLiteral(R"(answers da1=4 cpr=4(?!\d))"));

// The n-th session of the fast agent, from 1.
SessionKey Fast(int n) { return {FAST_AGENT, QStringLiteral("s%1").arg(n)}; }
SessionKey Slow() { return {SLOW_AGENT, QStringLiteral("s1")}; }

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

}  // namespace

class WarmTerminalsTest : public QObject {
    Q_OBJECT

private:
    // The plugin's agent.json gets an answer to agent.attachCommand for each of
    // its sessions s1 to s<count>: fake_tui, with the session as its greeting.
    bool AddAttach(const QString& plugin, int count, int delayMs) {
        QFile file(Daemon_->PluginsPath() + u'/' + plugin + "/agent.json");
        if (!file.open(QIODevice::ReadWrite)) {
            return false;
        }
        QJsonObject answers = QJsonDocument::fromJson(file.readAll()).object();
        QJsonObject forSessions;
        for (int n = 1; n <= count; ++n) {
            const QString id = QStringLiteral("s%1").arg(n);
            QJsonObject attach{
                {"result",
                 QJsonObject{{"program", FAKE_TUI_PATH},
                             // A short path: the greeting shares its row with
                             // the cwd, and must not wrap.
                             {"cwd", "/"},
                             {"env", QJsonObject{{"FAKE_TUI_GREETING",
                                                  plugin + u'-' + id}}}}}};
            if (delayMs > 0) {
                attach.insert("delayMs", delayMs);
            }
            forSessions.insert(plugin + "-agent/" + id, attach);
        }
        answers.insert("agent.attachCommand", forSessions);
        file.resize(0);
        return file.write(QJsonDocument(answers).toJson()) > 0;
    }

    // The daemon with both plugins, the IO thread connected to it, the pool,
    // and a pane that records its status changes.
    bool Prepare() {
        if (!Daemon_->IsValid() ||
            !Daemon_->AddFixturePlugin("fast", FIXTURE_BACKEND_PATH) ||
            !Daemon_->AddFixturePlugin("slow", FIXTURE_BACKEND_PATH) ||
            !AddAttach("fast", FAST_SESSIONS, 0) ||
            !AddAttach("slow", 1, SLOW_DELAY_MS) ||
            !Daemon_->Start(ROOSTD_PATH)) {
            return false;
        }
        Proxy_ = std::make_unique<CountingProxy>(
            QFileInfo(Daemon_->SocketPath()).dir().filePath("proxy.sock"),
            Daemon_->SocketPath());
        if (!Proxy_->Listen()) {
            return false;
        }
        Io_ = std::make_unique<IoThread>();
        if (!ConnectIo()) {
            return false;
        }
        Pool_ = std::make_unique<WarmTerminals>(*Io_);
        Pane_ = std::make_unique<SessionPane>(*Pool_);
        connect(Pane_.get(), &SessionPane::StatusChanged, Pane_.get(),
                [this] { Statuses_.push_back(Pane_->Status()); });
        return true;
    }

    bool ConnectIo() {
        QFuture<rpc::Result> hello = Io_->Connect(Proxy_->Path());
        return QTest::qWaitFor([&] { return hello.isFinished(); },
                               TIMEOUT_MS) &&
               hello.result();
    }

    // A request and its answer: what the IO thread sent before has reached
    // roostd, and what roostd sent before the answer has been delivered.
    bool RoundTrip() {
        QFuture<rpc::Result> round =
            Io_->Request(QStringLiteral("plugins.list"));
        if (!QTest::qWaitFor([&] { return round.isFinished(); }, TIMEOUT_MS)) {
            return false;
        }
        QCoreApplication::processEvents();
        return true;
    }

    // How many channels roostd still had to the session when it ended; see
    // ChannelsLeft.
    int ChannelsLeftTo(const SessionKey& key) {
        return ChannelsLeft(*Io_, key.Agent, key.Id, TIMEOUT_MS);
    }

    // Shows the session in the pane and waits until it is Open and the TUI has
    // had its queries answered: fake_tui reacts to a key only if the key
    // arrives in a read of its own.
    void ShowAndSettle(const SessionKey& key) {
        Pane_->Show(key);
        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Open,
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Pane_->Session()).contains(ANSWERED_ONCE), TIMEOUT_MS);
    }

    // The screen of a session the pane does not show, as the pool has it.
    QString ScreenOf(const SessionKey& key) {
        return Screen(Pool_->Session(key));
    }

    // Makes the session answer its queries once more, through a channel of
    // its own, so that nothing of the pool is involved: a second channel to
    // the same program, which writes to the same terminal.
    std::uint32_t OpenDirect(const SessionKey& key) {
        QFuture<OpenResult> open = Io_->Open(key.Agent, key.Id, 80, 24);
        if (!QTest::qWaitFor([&] { return open.isFinished(); }, TIMEOUT_MS) ||
            !open.result()) {
            return 0;
        }
        return open.result()->Channel;
    }

    std::unique_ptr<RoostdProcess> Daemon_;
    std::unique_ptr<CountingProxy> Proxy_;
    std::unique_ptr<IoThread> Io_;
    std::unique_ptr<WarmTerminals> Pool_;
    std::unique_ptr<SessionPane> Pane_;
    std::vector<EPaneStatus> Statuses_;

private slots:
    void init() {
        Daemon_ = std::make_unique<RoostdProcess>();
        Statuses_.clear();
    }

    void cleanup() {
        Pane_.reset();
        Pool_.reset();
        if (Io_) {
            Io_->Stop();
        }
        Io_.reset();
        Proxy_.reset();
        Daemon_.reset();
    }

    // The point of the pool: no open, no Opening, the same terminal.
    void SwitchingBackOpensNoChannel() {
        QVERIFY(Prepare());
        ShowAndSettle(Fast(1));
        const auto first = Pane_->Session();
        ShowAndSettle(Fast(2));
        QCOMPARE(Proxy_->OpenRequests(), 2);
        Statuses_.clear();

        Pane_->Show(Fast(1));

        // At once, in the same call.
        QCOMPARE(Pane_->Status(), EPaneStatus::Open);
        QCOMPARE(Pane_->Session(), first);
        QVERIFY(RoundTrip());
        QCOMPARE(Proxy_->OpenRequests(), 2);
        // The status stays Open: it passes through no Opening.
        QVERIFY(std::ranges::find(Statuses_, EPaneStatus::Opening) ==
                Statuses_.end());
        // The session it left is still the pool's: roostd has its channel.
        QCOMPARE(ChannelsLeftTo(Fast(2)), 1);
    }

    // The output of a hidden session lands in its terminal, so that the screen
    // is current when it is shown. A second channel to the program feeds it,
    // and the pool's channel only receives.
    void AHiddenSessionKeepsBeingFed() {
        QVERIFY(Prepare());
        ShowAndSettle(Fast(1));
        const auto hidden = Pane_->Session();
        ShowAndSettle(Fast(2));
        const std::uint32_t direct = OpenDirect(Fast(1));
        QVERIFY(direct != 0);
        QVERIFY(!Screen(hidden).contains(ANSWERED_TWICE));

        Io_->SendInput(direct, "q");

        QTRY_VERIFY_WITH_TIMEOUT(Screen(hidden).contains(ANSWERED_TWICE),
                                 TIMEOUT_MS);
        // And it is the screen the switch shows.
        Pane_->Show(Fast(1));
        QCOMPARE(Pane_->Session(), hidden);
        QVERIFY(Screen(Pane_->Session()).contains(ANSWERED_TWICE));
    }

    // IoThread raises Updated again only after an acknowledgement: a hidden
    // channel that is never acknowledged raises at most one, however much
    // arrives.
    void AHiddenSessionRaisesNoUpdates() {
        QVERIFY(Prepare());
        ShowAndSettle(Fast(1));
        const auto hidden = Pane_->Session();
        const std::uint32_t channel = Pool_->Channel(Fast(1));
        ShowAndSettle(Fast(2));
        QVERIFY(RoundTrip());
        int updates = 0;
        QObject scope;
        connect(Io_.get(), &IoThread::Updated, &scope,
                [&updates, channel](std::uint32_t changed) {
                    if (changed == channel) {
                        ++updates;
                    }
                });
        const std::uint32_t direct = OpenDirect(Fast(1));
        QVERIFY(direct != 0);

        // Three rounds of output, each seen on the screen and delivered
        // before the next starts.
        for (int round = 1; round <= 3; ++round) {
            Io_->SendInput(direct, "q");
            const QRegularExpression answered(
                QStringLiteral("answers da1=%1 ").arg(round + 1));
            QTRY_VERIFY_WITH_TIMEOUT(Screen(hidden).contains(answered),
                                     TIMEOUT_MS);
            QVERIFY(RoundTrip());
        }

        QVERIFY(Screen(hidden).contains(ANSWERED_FOUR_TIMES));
        QVERIFY2(updates <= 1, qPrintable(QString::number(updates)));
    }

    // The window is what makes a switch count for roostd's LRU.
    void AWarmSwitchSendsAView() {
        QVERIFY(Prepare());
        ShowAndSettle(Fast(1));
        ShowAndSettle(Fast(2));
        // An open is a touch of its own.
        QVERIFY(RoundTrip());
        QCOMPARE(Proxy_->ViewRequests(), 0);

        Pane_->Show(Fast(1));

        QVERIFY(RoundTrip());
        QCOMPARE(Proxy_->ViewRequests(), 1);
        Pane_->Show(Fast(2));
        QVERIFY(RoundTrip());
        QCOMPARE(Proxy_->ViewRequests(), 2);
    }

    // Five sessions against roostd's four. The window has no count of its own:
    // it holds what roostd holds. Going back to the first before the fifth
    // makes the second the one roostd evicts, which only the views tell it.
    void TheWindowDropsExactlyWhatTheDaemonEvicted() {
        QVERIFY(Prepare());
        for (int n = 1; n <= DAEMON_CAPACITY; ++n) {
            ShowAndSettle(Fast(n));
        }
        QCOMPARE(Pool_->Size(), std::size_t{DAEMON_CAPACITY});
        Pane_->Show(Fast(1));
        QCOMPARE(Pane_->Status(), EPaneStatus::Open);
        QVERIFY(RoundTrip());

        Pane_->Show(Fast(FAST_SESSIONS));

        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Open,
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Pool_->Status(Fast(2)), EPaneStatus::None,
                                  TIMEOUT_MS);
        QCOMPARE(Pool_->Size(), std::size_t{DAEMON_CAPACITY});
        for (const int n : {1, 3, 4, FAST_SESSIONS}) {
            QCOMPARE(Pool_->Status(Fast(n)), EPaneStatus::Open);
        }
        // The one that was pushed out opens anew.
        Pane_->Show(Fast(2));
        QCOMPARE(Pane_->Status(), EPaneStatus::Opening);
    }

    void AHiddenSessionThatEndsIsDropped() {
        QVERIFY(Prepare());
        ShowAndSettle(Fast(1));
        const auto hidden = Pane_->Session();
        ShowAndSettle(Fast(2));

        // Ends fast/s1 through a channel of its own; the hidden channel of the
        // pool is closed too.
        QCOMPARE(ChannelsLeftTo(Fast(1)), 1);

        QCOMPARE(Pool_->Status(Fast(1)), EPaneStatus::None);
        QCOMPARE(Pool_->Size(), std::size_t{1});
        QCOMPARE(Pane_->Status(), EPaneStatus::Open);
        // Showing it again is an open of a new terminal, not a dead screen.
        Pane_->Show(Fast(1));
        QCOMPARE(Pane_->Status(), EPaneStatus::Opening);
        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Open,
                                  TIMEOUT_MS);
        QVERIFY(Pane_->Session() != hidden);
    }

    void AShownSessionThatEndsKeepsItsScreen() {
        QVERIFY(Prepare());
        ShowAndSettle(Fast(1));
        const auto session = Pane_->Session();

        // Ends fast/s1; the channel of the pool is closed too.
        QCOMPARE(ChannelsLeftTo(Fast(1)), 1);

        QCOMPARE(Pane_->Status(), EPaneStatus::Ended);
        QCOMPARE(Pane_->Message(), QStringLiteral("exited with code 0"));
        QCOMPARE(Pane_->Session(), session);
        QVERIFY(Screen(session).contains("fake tui"));
        QCOMPARE(Pool_->Size(), std::size_t{1});
        // Hidden after that, it is dropped: an ended session is not warm.
        Pane_->Show(std::nullopt);
        QCOMPARE(Pool_->Size(), std::size_t{0});
    }

    void ALostConnectionDropsHiddenAndReopensOnlyTheShown() {
        QVERIFY(Prepare());
        ShowAndSettle(Fast(1));
        ShowAndSettle(Fast(2));
        const auto shown = Pane_->Session();
        QCOMPARE(Proxy_->OpenRequests(), 2);

        QVERIFY(Daemon_->Stop());

        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Disconnected,
                                  TIMEOUT_MS);
        QCOMPARE(Pane_->Session(), shown);
        QVERIFY(Screen(shown).contains("fake tui"));
        QCOMPARE(Pool_->Status(Fast(1)), EPaneStatus::None);
        QCOMPARE(Pool_->Size(), std::size_t{1});

        // The window's Api connects again and says so.
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(ConnectIo());
        Pool_->ReopenShown();

        QCOMPARE(Pane_->Status(), EPaneStatus::Opening);
        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Open,
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Pane_->Session()).contains("greeting=fast-s2"), TIMEOUT_MS);
        QVERIFY(RoundTrip());
        // One open for the shown session, none for the hidden one.
        QCOMPARE(Proxy_->OpenRequests(), 3);
        QCOMPARE(Pool_->Size(), std::size_t{1});
    }

    // Hidden, the PTY keeps the size it has; shown, it gets one resize, and
    // only if the size differs.
    void AWarmSessionGetsOneResizeOnShowAndNoneWhileHidden() {
        QVERIFY(Prepare());
        Pane_->SetGrid(80, 24);
        ShowAndSettle(Fast(1));
        ShowAndSettle(Fast(2));
        QCOMPARE(Proxy_->ResizeRequests(), 0);

        // The window grows while fast/s2 is shown: fast/s1 is hidden.
        Pane_->SetGrid(100, 30);
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Pane_->Session()).contains(SizeLine(100, 30)), TIMEOUT_MS);
        QVERIFY(RoundTrip());
        QCOMPARE(Proxy_->ResizeRequests(), 1);
        QVERIFY(ScreenOf(Fast(1)).contains(SizeLine(80, 24)));

        Pane_->Show(Fast(1));

        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Pane_->Session()).contains(SizeLine(100, 30)), TIMEOUT_MS);
        QVERIFY(RoundTrip());
        QCOMPARE(Proxy_->ResizeRequests(), 2);
        // Both are at the size now: another switch sends nothing.
        Pane_->Show(Fast(2));
        QVERIFY(RoundTrip());
        QCOMPARE(Proxy_->ResizeRequests(), 2);
    }

    // Warm again, a session is acknowledged once more: the one Updated that a
    // hidden channel raised is not acknowledged, so without the
    // acknowledgement of the show it would never raise another.
    void AWarmShowAcknowledgesTheUpdateRaisedWhileHidden() {
        QVERIFY(Prepare());
        ShowAndSettle(Fast(1));
        const auto hidden = Pane_->Session();
        ShowAndSettle(Fast(2));
        QVERIFY(RoundTrip());
        const std::uint32_t direct = OpenDirect(Fast(1));
        QVERIFY(direct != 0);
        // Output while hidden: the channel raises its one Updated, which the
        // pool does not acknowledge.
        Io_->SendInput(direct, "q");
        QTRY_VERIFY_WITH_TIMEOUT(Screen(hidden).contains(ANSWERED_TWICE),
                                 TIMEOUT_MS);
        QVERIFY(RoundTrip());
        int updates = 0;
        connect(Pane_.get(), &SessionPane::Updated, Pane_.get(),
                [&updates] { ++updates; });

        Pane_->Show(Fast(1));
        QVERIFY(RoundTrip());
        const int before = updates;
        Io_->SendInput(direct, "q");

        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Pane_->Session()).contains(QStringLiteral("answers da1=3 ")),
            TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(updates > before, TIMEOUT_MS);
    }

    // A new pane for a session another pane still shows (a QML Loader makes
    // the new item before the old one is deleted): the newest takes the
    // session over, the old one presents nothing, and the old one's end does
    // not stop the new one's updates.
    void ASecondPaneTakesTheSessionOver() {
        QVERIFY(Prepare());
        ShowAndSettle(Fast(1));
        const auto session = Pane_->Session();
        auto second = std::make_unique<SessionPane>(*Pool_);
        int updates = 0;
        connect(second.get(), &SessionPane::Updated, second.get(),
                [&updates] { ++updates; });

        second->Show(Fast(1));

        QCOMPARE(second->Status(), EPaneStatus::Open);
        QCOMPARE(second->Session(), session);
        QVERIFY(!Pane_->Session());
        QCOMPARE(Pane_->Status(), EPaneStatus::None);
        QCOMPARE(Pool_->Owner(Fast(1)),
                 static_cast<const QObject*>(second.get()));
        // The old pane's end is not the new one's.
        Pane_.reset();
        QVERIFY(Pool_->IsShown(Fast(1)));
        QCOMPARE(Pool_->Owner(Fast(1)),
                 static_cast<const QObject*>(second.get()));
        QCOMPARE(second->Status(), EPaneStatus::Open);
        const std::uint32_t direct = OpenDirect(Fast(1));
        QVERIFY(direct != 0);
        Io_->SendInput(direct, "q");
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(second->Session()).contains(ANSWERED_TWICE), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(updates > 0, TIMEOUT_MS);
        const int before = updates;
        Io_->SendInput(direct, "q");
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(second->Session())
                .contains(QStringLiteral("answers da1=3 ")),
            TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(updates > before, TIMEOUT_MS);
    }

    void DestroyingThePoolClosesEveryChannel() {
        QVERIFY(Prepare());
        ShowAndSettle(Fast(1));
        ShowAndSettle(Fast(2));
        // And one that is still opening.
        Pane_->Show(Slow());
        QCOMPARE(Pane_->Status(), EPaneStatus::Opening);
        QCOMPARE(Pool_->Size(), std::size_t{3});

        Pane_.reset();
        Pool_.reset();

        QCOMPARE(ChannelsLeftTo(Fast(1)), 0);
        QCOMPARE(ChannelsLeftTo(Fast(2)), 0);
        // The slow open was asked first, so its answer has been dealt with
        // when a channel opened now to the same session is answered.
        QCOMPARE(ChannelsLeftTo(Slow()), 0);
    }

    void ADestroyedPaneLeavesTheSessionWarm() {
        QVERIFY(Prepare());
        ShowAndSettle(Fast(1));
        const auto session = Pane_->Session();
        const std::uint32_t channel = Pool_->Channel(Fast(1));

        Pane_.reset();

        QVERIFY(!Pool_->IsShown(Fast(1)));
        QCOMPARE(Pool_->Status(Fast(1)), EPaneStatus::Open);
        QCOMPARE(Pool_->Session(Fast(1)), session);
        QCOMPARE(Pool_->Channel(Fast(1)), channel);
        // A pane of another panel shows it at once.
        Pane_ = std::make_unique<SessionPane>(*Pool_);
        Pane_->Show(Fast(1));
        QCOMPARE(Pane_->Status(), EPaneStatus::Open);
        QCOMPARE(Pane_->Session(), session);
        QVERIFY(RoundTrip());
        QCOMPARE(Proxy_->OpenRequests(), 1);
    }

    // The answer of an open the pane has left comes into the pool as a warm
    // session, hidden, and showing it later is a switch.
    void AnOpenInFlightWhenThePaneMovesOnCompletesIntoAHiddenEntry() {
        QVERIFY(Prepare());
        Pane_->Show(Slow());
        QCOMPARE(Pane_->Status(), EPaneStatus::Opening);

        ShowAndSettle(Fast(1));

        QTRY_COMPARE_WITH_TIMEOUT(Pool_->Status(Slow()), EPaneStatus::Open,
                                  TIMEOUT_MS);
        QVERIFY(!Pool_->IsShown(Slow()));
        const auto session = Pool_->Session(Slow());
        QVERIFY(session);
        Pane_->Show(Slow());
        QCOMPARE(Pane_->Status(), EPaneStatus::Open);
        QCOMPARE(Pane_->Session(), session);
        QVERIFY(RoundTrip());
        QCOMPARE(Proxy_->OpenRequests(), 2);
    }

    // An open that is refused leaves nothing warm behind, whether the pane
    // waited for the answer or not.
    void AFailedOpenIsDroppedWhenHidden() {
        QVERIFY(Prepare());
        const SessionKey unknown{"nope", "s1"};
        Pane_->Show(unknown);
        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Failed,
                                  TIMEOUT_MS);
        QCOMPARE(Pool_->Size(), std::size_t{1});

        ShowAndSettle(Fast(1));
        QCOMPARE(Pool_->Size(), std::size_t{1});

        // Not waited for: hidden when the refusal comes.
        Pane_->Show(unknown);
        Pane_->Show(Fast(1));
        QCOMPARE(Pane_->Status(), EPaneStatus::Open);
        QTRY_COMPARE_WITH_TIMEOUT(Pool_->Size(), std::size_t{1}, TIMEOUT_MS);
        QCOMPARE(Pool_->Status(unknown), EPaneStatus::None);
    }
};

QTEST_MAIN(WarmTerminalsTest)
#include "warm_terminals_test.moc"
