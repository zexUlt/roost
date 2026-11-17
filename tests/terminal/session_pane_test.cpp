// SessionPane against the real roostd with two fixture plugins, whose agents
// "fast-agent" and "slow-agent" attach to fake_tui (the slow one answers
// agent.attachCommand late): what the pane shows and which status it reports
// for an open, a quick switch, the end of the session, a refused open, a
// null, a burst of sizes and a lost connection.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>
#include <QtEndian>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "core/frame.hpp"
#include "helpers/channels_left.hpp"
#include "helpers/roostd_process.hpp"
#include "terminal/io_thread.hpp"
#include "terminal/session_pane.hpp"

using namespace roost;
using roost::testing::ChannelsLeft;
using roost::testing::RoostdProcess;

namespace {

constexpr int TIMEOUT_MS = 10000;
// How long the slow plugin holds back its answer to agent.attachCommand.
constexpr int SLOW_DELAY_MS = 300;
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

// A socket between the IO thread and roostd that passes the bytes through and
// counts what the client sends: session.resize requests and input frames.
// Runs in the thread that makes it.
class CountingProxy : public QObject {
public:
    CountingProxy(QString listenPath, QString upstreamPath)
        : Listen_(std::move(listenPath)), Upstream_(std::move(upstreamPath)) {
        connect(&Server_, &QLocalServer::newConnection, this,
                &CountingProxy::Accept);
    }

    bool Listen() { return Server_.listen(Listen_); }
    const QString& Path() const { return Listen_; }
    int ResizeRequests() const { return ResizeRequests_; }
    int InputFrames() const { return InputFrames_; }

private:
    void Accept() {
        while (QLocalSocket* const client = Server_.nextPendingConnection()) {
            auto* const upstream = new QLocalSocket(client);
            upstream->connectToServer(Upstream_);
            if (!upstream->waitForConnected(TIMEOUT_MS)) {
                client->disconnectFromServer();
                continue;
            }
            const auto buffer = std::make_shared<QByteArray>();
            connect(client, &QLocalSocket::readyRead, client,
                    [this, client, upstream, buffer] {
                        const QByteArray bytes = client->readAll();
                        upstream->write(bytes);
                        buffer->append(bytes);
                        Count(*buffer);
                    });
            connect(upstream, &QLocalSocket::readyRead, client,
                    [client, upstream] { client->write(upstream->readAll()); });
            connect(client, &QLocalSocket::disconnected, upstream,
                    [upstream] { upstream->disconnectFromServer(); });
            connect(upstream, &QLocalSocket::disconnected, client,
                    [client] { client->disconnectFromServer(); });
        }
    }

    // Takes the whole frames off the front of what the client sent.
    void Count(QByteArray& buffer) {
        constexpr qsizetype lengthSize = 4;
        while (buffer.size() > lengthSize) {
            const auto length = qFromBigEndian<quint32>(buffer.constData());
            const qsizetype frame = lengthSize + qsizetype{length};
            if (length == 0 || buffer.size() < frame) {
                return;
            }
            const auto kind = static_cast<EFrameKind>(buffer[lengthSize]);
            if (kind == EFrameKind::Input) {
                ++InputFrames_;
            } else if (kind == EFrameKind::Rpc &&
                       QByteArrayView(buffer)
                           .sliced(lengthSize, length)
                           .contains("\"session.resize\"")) {
                ++ResizeRequests_;
            }
            buffer.remove(0, frame);
        }
    }

    QLocalServer Server_;
    QString Listen_;
    QString Upstream_;
    int ResizeRequests_ = 0;
    int InputFrames_ = 0;
};

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
        Pane_ = std::make_unique<SessionPane>(*Io_);
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
    std::unique_ptr<CountingProxy> Proxy_;
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
        Proxy_.reset();
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
        // The slow open was asked first, so its answer has been dealt with
        // when a channel opened now to the same session is answered; roostd
        // must have no channel left for it.
        QCOMPARE(ChannelsLeftTo(Slow()), 0);
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
        // Input after the end goes nowhere: not even a frame is sent.
        QVERIFY(RoundTrip());
        const int inputFrames = Proxy_->InputFrames();
        Pane_->SendInput("q");
        QVERIFY(RoundTrip());
        QCOMPARE(Proxy_->InputFrames(), inputFrames);
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
        // The screen is parsed on the IO thread; Updated reaches this thread
        // later.
        QTRY_VERIFY_WITH_TIMEOUT(!Channels_.empty(), TIMEOUT_MS);
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
        QVERIFY(RoundTrip());
        QVERIFY(!Screen(old).contains(QRegularExpression("da1=3")));
        QVERIFY(Screen(old).contains(ANSWERED_TWICE));
        // roostd has no channel left to the session.
        QCOMPARE(ChannelsLeftTo(Fast()), 0);
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
        Pane_->SetGrid(80, 25);
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Pane_->Session()).contains(SizeLine(80, 25)), TIMEOUT_MS);
        const int before = Proxy_->ResizeRequests();

        // The tail without the event loop: the first of these is in flight
        // while the others arrive, so only the latest may be sent after it.
        Pane_->SetGrid(90, 28);
        Pane_->SetGrid(95, 30);
        Pane_->SetGrid(97, 31);

        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Pane_->Session()).contains(SizeLine(97, 31)), TIMEOUT_MS);
        QCOMPARE(Pane_->Status(), EPaneStatus::Open);
        // The first, and the latest after its answer; never one per size. (The
        // first waits, too, if the answer to the size before is still on its
        // way.)
        QVERIFY(RoundTrip());
        const int sent = Proxy_->ResizeRequests() - before;
        QVERIFY2(sent >= 1 && sent <= 2, qPrintable(QString::number(sent)));
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

    // The control of the checks below: the channel of an open pane is counted.
    void TheChannelOfAnOpenPaneIsCounted() {
        QVERIFY(Prepare());
        OpenFastAndSettle();

        QCOMPARE(ChannelsLeftTo(Fast()), 1);
    }

    void TheDestructorClosesTheChannel() {
        QVERIFY(Prepare());
        OpenFastAndSettle();

        Pane_.reset();

        QCOMPARE(ChannelsLeftTo(Fast()), 0);
    }

    void SwitchingFromAnOpenSessionClosesItsChannel() {
        QVERIFY(Prepare());
        OpenFastAndSettle();

        Pane_->Show(Slow());

        QCOMPARE(ChannelsLeftTo(Fast()), 0);
        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Open,
                                  TIMEOUT_MS);
        QVERIFY(Pane_->Key() == std::optional<SessionKey>(Slow()));
    }

    void AResizeDuringTheOpenIsApplied() {
        QVERIFY(Prepare());
        Pane_->SetGrid(80, 24);

        Pane_->Show(Slow());
        Pane_->SetGrid(100, 30);

        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Open,
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Pane_->Session()).contains(SizeLine(100, 30)), TIMEOUT_MS);
    }

    // The session of the pane before the switch ends: its Closed is not the
    // pane's.
    void TheEndOfAnotherChannelLeavesTheStatusAlone() {
        QVERIFY(Prepare());
        OpenFastAndSettle();
        Pane_->Show(Slow());
        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Open,
                                  TIMEOUT_MS);
        const auto session = Pane_->Session();
        const auto statusChanges = Statuses_.size();

        // Ends fast/s1 through a channel of its own.
        QCOMPARE(ChannelsLeftTo(Fast()), 0);

        QCOMPARE(Pane_->Status(), EPaneStatus::Open);
        QCOMPARE(Pane_->Session(), session);
        QCOMPARE(Statuses_.size(), statusChanges);
    }

    // SessionChanged is how the item gets the session: it comes before the
    // status Open, so a status handler that sees Open already has it.
    void TheSessionIsAnnouncedBeforeTheStatusBecomesOpen() {
        QVERIFY(Prepare());
        QSignalSpy sessionChanged(Pane_.get(), &SessionPane::SessionChanged);
        QObject scope;
        std::vector<qsizetype> announcedAtOpen;
        connect(Pane_.get(), &SessionPane::StatusChanged, &scope, [&] {
            if (Pane_->Status() == EPaneStatus::Open) {
                announcedAtOpen.push_back(sessionChanged.count());
            }
        });

        Pane_->Show(Fast());

        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Open,
                                  TIMEOUT_MS);
        const std::vector<qsizetype> expected{1};
        QCOMPARE(announcedAtOpen, expected);
    }

    // The channel is silent after each Updated until the pane acknowledges
    // it: later output must still arrive.
    void UpdatesKeepComingAfterTheFirst() {
        QVERIFY(Prepare());
        OpenFastAndSettle();
        QSignalSpy updated(Pane_.get(), &SessionPane::Updated);

        for (int round = 1; round <= 3; ++round) {
            const auto before = updated.count();
            Pane_->SendInput("q");
            QTRY_VERIFY_WITH_TIMEOUT(updated.count() > before, TIMEOUT_MS);
        }
        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(Pane_->Session()).contains(QRegularExpression("da1=4")),
            TIMEOUT_MS);
    }

    // The late channel must be closed and nothing of the pane touched.
    void APaneGoneWhileOpeningLeavesNoCrash() {
        QVERIFY(Prepare());
        Pane_->Show(Slow());
        QCOMPARE(Pane_->Status(), EPaneStatus::Opening);

        Pane_.reset();

        // The pane's open was asked first, so its answer has been dealt with
        // when this one is answered.
        QCOMPARE(ChannelsLeftTo(Slow()), 0);
        // The IO thread still serves a new pane.
        Pane_ = std::make_unique<SessionPane>(*Io_);
        Pane_->Show(Fast());
        QTRY_COMPARE_WITH_TIMEOUT(Pane_->Status(), EPaneStatus::Open,
                                  TIMEOUT_MS);
    }
};

QTEST_MAIN(SessionPaneTest)
#include "session_pane_test.moc"
