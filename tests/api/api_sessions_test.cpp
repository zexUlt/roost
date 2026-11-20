// The session cache of the window against the real roostd, with a plugin
// whose backend is the fixture backend: the rows of sessions.list, the
// session.stateChanged events that change and add rows, the session.removed
// events that drop them, the agents that did
// not answer, Refresh, and what the cache does when the connection is lost.

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <memory>
#include <vector>

#include "api/api.hpp"
#include "api/thread_check.hpp"
#include "api_test_support.hpp"
#include "core/protocol.hpp"
#include "helpers/roostd_process.hpp"

using namespace roost;
using namespace roost::api_test;

namespace {

constexpr int CONNECTION_LOST =
    static_cast<int>(protocol::EErrorCode::ConnectionLost);

const QString AGENT = QStringLiteral("fixture-agent");

QJsonObject Row(const QString& id, const QString& state,
                const QString& title = {}) {
    return {{"agent", AGENT},
            {"id", id},
            {"title", title},
            {"cwd", ""},
            {"state", state}};
}

// agent.json of the fixture plugin: what its backend answers to
// agent.listSessions, read anew for each request.
QJsonObject Answers(const QJsonArray& sessions) {
    return {{"agent.listSessions",
             QJsonObject{{AGENT, QJsonObject{{"result", sessions}}}}}};
}

QJsonObject StateChanged(const QJsonObject& session) {
    return {{"jsonrpc", "2.0"},
            {"method", "session.stateChanged"},
            {"params", QJsonObject{{"agent", AGENT}, {"session", session}}}};
}

QJsonObject Removed(const QString& id) {
    return {{"jsonrpc", "2.0"},
            {"method", "session.removed"},
            {"params", QJsonObject{{"agent", AGENT}, {"session", id}}}};
}

}  // namespace

class ApiSessionsTest : public QObject {
    Q_OBJECT

private:
    QString AgentFile() const {
        return Daemon_->PluginsPath() + "/fixture/agent.json";
    }

    bool SetAnswers(const QJsonObject& answers) {
        QFile file(AgentFile());
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               file.write(QJsonDocument(answers).toJson()) > 0;
    }

    // The daemon, with the sessions s1 (idle) and s2 (working).
    bool StartDaemon() {
        return Daemon_->IsValid() &&
               Daemon_->AddFixturePlugin("fixture", FIXTURE_BACKEND_PATH) &&
               SetAnswers(Answers({
                   Row("s1", "idle"),
                   QJsonObject{{"agent", AGENT},
                               {"id", "s2"},
                               {"title", "second"},
                               {"cwd", "/tmp"},
                               {"state", "working"},
                               {"startedAt", qint64{1700000000000}}},
               })) &&
               Daemon_->Start(ROOSTD_PATH);
    }

    // The daemon with a second plugin, "slow", whose agent "slow-agent" is the
    // one that can be made to answer late (SetSlowListing).
    bool StartDaemonWithASlowAgent() {
        return Daemon_->IsValid() &&
               Daemon_->AddFixturePlugin("slow", FIXTURE_BACKEND_PATH) &&
               StartDaemon();
    }

    // The slow agent answers its next list after delayMs.
    bool SetSlowListing(int delayMs) {
        return WriteSlowListing(QJsonObject{{"delayMs", delayMs}});
    }

    // The slow agent answers its next list when ReleaseSlowListing is called:
    // for a case that must have the list out until something is seen.
    bool HoldSlowListing() {
        return WriteSlowListing(QJsonObject{{"holdUntilFile", GatePath()}});
    }

    bool ReleaseSlowListing() {
        QFile gate(GatePath());
        return gate.open(QIODevice::WriteOnly);
    }

    QString GatePath() const {
        return Daemon_->PluginsPath() + "/slow/release";
    }

    bool WriteSlowListing(QJsonObject hold) {
        QFile file(Daemon_->PluginsPath() + "/slow/agent.json");
        hold.insert("result", QJsonArray{});
        const QJsonObject answers{
            {"agent.listSessions", QJsonObject{{"slow-agent", hold}}}};
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               file.write(QJsonDocument(answers).toJson()) > 0;
    }

    // Connected, and the first list is in.
    bool ConnectApi() {
        Api_ = std::make_unique<api::Api>();
        Watch();
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        Api_->Connect(Daemon_->SocketPath());
        // The signal may be said inside Connect: look at the spy before
        // waiting.
        return (!connected.isEmpty() || connected.wait(TIMEOUT_MS)) &&
               QTest::qWaitFor([this] { return Api_->Sessions().IsLoaded(); },
                               TIMEOUT_MS);
    }

    // Has the backend send these notifications to roostd.
    bool Notify(const QJsonArray& messages) {
        const QFuture<rpc::Result> sent =
            Api_->Commands().Run("fixture.notify", messages);
        return WaitFor(sent) && sent.result();
    }

    // Records the thread of every signal, and the order of the row signals.
    void Watch() {
        const api::SessionService* sessions = &Api_->Sessions();
        connect(sessions, &api::SessionService::Reset, this,
                [this] { Threads_.push_back(QThread::currentThread()); });
        connect(sessions, &api::SessionService::RowChanged, this,
                [this](int) { Threads_.push_back(QThread::currentThread()); });
        connect(sessions, &api::SessionService::RowInserted, this,
                [this](int) { Threads_.push_back(QThread::currentThread()); });
        connect(sessions, &api::SessionService::RowAboutToBeRemoved, this,
                [this](int) { Threads_.push_back(QThread::currentThread()); });
        connect(sessions, &api::SessionService::RowRemoved, this,
                [this](int) { Threads_.push_back(QThread::currentThread()); });
    }

    std::unique_ptr<roost::testing::RoostdProcess> Daemon_;
    std::unique_ptr<api::Api> Api_;
    std::vector<QThread*> Threads_;

private slots:
    void init() {
        Daemon_ = std::make_unique<roost::testing::RoostdProcess>();
        Threads_.clear();
    }

    void cleanup() {
        Api_.reset();
        Daemon_.reset();
    }

    void TheRowsAreTheListOfRoostdInItsOrder() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        const auto& sessions = Api_->Sessions();
        QCOMPARE(sessions.Rows().size(), 2);
        QCOMPARE(sessions.Rows()[0], Row("s1", "idle"));
        QCOMPARE(sessions.Rows()[1],
                 (QJsonObject{{"agent", AGENT},
                              {"id", "s2"},
                              {"title", "second"},
                              {"cwd", "/tmp"},
                              {"state", "working"},
                              {"startedAt", qint64{1700000000000}}}));
        QCOMPARE(sessions.IndexOf(AGENT, "s1"), 0);
        QCOMPARE(sessions.IndexOf(AGENT, "s2"), 1);
        QCOMPARE(sessions.IndexOf(AGENT, "s3"), -1);
        QCOMPARE(sessions.IndexOf("other", "s1"), -1);
        QVERIFY(sessions.Unavailable().isEmpty());
        // The first list is announced as a reset, in the GUI thread.
        QCOMPARE(Threads_, std::vector<QThread*>{QThread::currentThread()});
    }

    void AStateChangeReplacesAKnownRow() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        QSignalSpy changed(&Api_->Sessions(), &api::SessionService::RowChanged);
        QSignalSpy inserted(&Api_->Sessions(),
                            &api::SessionService::RowInserted);
        const QJsonObject done = Row("s2", "done", "finished");
        QVERIFY(Notify({StateChanged(done)}));
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, TIMEOUT_MS);
        QCOMPARE(changed[0][0].toInt(), 1);
        QVERIFY(inserted.isEmpty());
        // The whole row is replaced: startedAt of the old one is gone.
        QCOMPARE(Api_->Sessions().Rows().size(), 2);
        QCOMPARE(Api_->Sessions().Rows()[1], done);
        QCOMPARE(Api_->Sessions().Rows()[0], Row("s1", "idle"));
        for (QThread* thread : Threads_) {
            QCOMPARE(thread, QThread::currentThread());
        }
    }

    void AnUnknownSessionIsAppended() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        QSignalSpy changed(&Api_->Sessions(), &api::SessionService::RowChanged);
        QSignalSpy inserted(&Api_->Sessions(),
                            &api::SessionService::RowInserted);
        const QJsonObject added = Row("s3", "blocked", "new");
        QVERIFY(Notify({StateChanged(added)}));
        QTRY_COMPARE_WITH_TIMEOUT(inserted.size(), 1, TIMEOUT_MS);
        QCOMPARE(inserted[0][0].toInt(), 2);
        QVERIFY(changed.isEmpty());
        QCOMPARE(Api_->Sessions().Rows().size(), 3);
        QCOMPARE(Api_->Sessions().Rows()[2], added);
        QCOMPARE(Api_->Sessions().IndexOf(AGENT, "s3"), 2);
        // Said again, it is the same row.
        QVERIFY(Notify({StateChanged(Row("s3", "done", "new"))}));
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, TIMEOUT_MS);
        QCOMPARE(changed[0][0].toInt(), 2);
        QCOMPARE(Api_->Sessions().Rows().size(), 3);
    }

    void ARemovedSessionIsDropped() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        const auto& sessions = Api_->Sessions();
        // What the rows were when each signal came: the row is still there
        // when it is about to go, and gone when it has.
        QStringList told;
        QObject scope;
        connect(
            &sessions, &api::SessionService::RowAboutToBeRemoved, &scope,
            [&](int index) {
                told.append(
                    QStringLiteral("about %1 of %2 (%3)")
                        .arg(index)
                        .arg(sessions.Rows().size())
                        .arg(sessions.Rows()[index].value("id").toString()));
            });
        connect(&sessions, &api::SessionService::RowRemoved, &scope,
                [&](int index) {
                    told.append(QStringLiteral("removed %1 of %2")
                                    .arg(index)
                                    .arg(sessions.Rows().size()));
                });
        QVERIFY(Notify({Removed("s1")}));
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 2, TIMEOUT_MS);
        QCOMPARE(told, (QStringList{"about 0 of 2 (s1)", "removed 0 of 1"}));
        QCOMPARE(sessions.Rows().size(), 1);
        QCOMPARE(sessions.Rows()[0].value("id"), QJsonValue("s2"));
        // The rows after it moved up.
        QCOMPARE(sessions.IndexOf(AGENT, "s2"), 0);
        QCOMPARE(sessions.IndexOf(AGENT, "s1"), -1);
        for (QThread* thread : Threads_) {
            QCOMPARE(thread, QThread::currentThread());
        }

        // A session that is not in the rows is no signal, and takes no row
        // with it: checked while a second row is there to be taken. The events
        // come in order, so the removals of the known sessions after it show
        // the first was handled, and told has nothing of it.
        QVERIFY(Notify({StateChanged(Row("s1", "idle"))}));
        QTRY_COMPARE_WITH_TIMEOUT(sessions.Rows().size(), 2, TIMEOUT_MS);
        told.clear();
        QVERIFY(Notify({Removed("s9"), Removed("s1"), Removed("s2")}));
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 4, TIMEOUT_MS);
        QCOMPARE(told, (QStringList{"about 1 of 2 (s1)", "removed 1 of 1",
                                    "about 0 of 1 (s2)", "removed 0 of 0"}));
        QVERIFY(sessions.Rows().isEmpty());
    }

    // roostd answers sessions.list when every agent has: the fast agent's part
    // is taken at once, and the slow agent holds the answer back. Its session
    // is said to be done meanwhile, and the answer still has it working.
    void AnEventDuringARefreshSurvivesAnOlderAnswer() {
        QVERIFY(StartDaemonWithASlowAgent());
        QVERIFY(ConnectApi());
        const int before = Api_->Sessions().IndexOf(AGENT, "s2");
        QVERIFY(before >= 0);
        QCOMPARE(Api_->Sessions().Rows()[before].value("state"),
                 QJsonValue("working"));
        QVERIFY(HoldSlowListing());
        QSignalSpy reset(&Api_->Sessions(), &api::SessionService::Reset);
        QSignalSpy changed(&Api_->Sessions(), &api::SessionService::RowChanged);
        const QFuture<rpc::Result> refreshed = Api_->Sessions().Refresh();
        // The list of the fast agent is asked for as soon as roostd has the
        // request; the event cannot come before it is, as the backend of the
        // fast agent speaks of one thing at a time.
        const QJsonObject done = Row("s2", "done", "finished");
        QVERIFY(Notify({StateChanged(done)}));
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, TIMEOUT_MS);
        QVERIFY(!refreshed.isFinished());
        QVERIFY(ReleaseSlowListing());
        QVERIFY(WaitFor(refreshed));
        QVERIFY2(refreshed.result(), qPrintable(Why(refreshed.result())));
        QCOMPARE(reset.size(), 1);
        const int index = Api_->Sessions().IndexOf(AGENT, "s2");
        QVERIFY(index >= 0);
        QCOMPARE(Api_->Sessions().Rows()[index], done);
        // The answer's own row for s2, replaced by the event again: told as a
        // change, after the reset.
        QCOMPARE(changed.size(), 2);
        QCOMPARE(changed[1][0].toInt(), index);
    }

    // What the adapter did while it was down is told by no event: the restart
    // is announced, and the window lists again.
    void ARestartedAdapterIsListedAnew() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        QVERIFY(Api_->Sessions().IndexOf(AGENT, "s1") >= 0);
        QSignalSpy reset(&Api_->Sessions(), &api::SessionService::Reset);
        QVERIFY(SetAnswers(Answers({Row("s2", "done")})));
        const QFuture<rpc::Result> crashed =
            Api_->Commands().Run("fixture.crash");
        QVERIFY(WaitFor(crashed));
        QTRY_COMPARE_WITH_TIMEOUT(Api_->Sessions().IndexOf(AGENT, "s1"), -1,
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Api_->Sessions().IndexOf(AGENT, "s2") >= 0,
                                 TIMEOUT_MS);
        const int index = Api_->Sessions().IndexOf(AGENT, "s2");
        QCOMPARE(Api_->Sessions().Rows()[index], Row("s2", "done"));
        QCOMPARE(Api_->Sessions().Rows().size(), 1);
        QCOMPARE(reset.size(), 1);
    }

    // The event of the old process that a list is out for is older than the
    // restart: the list that follows the restart is the one to trust.
    void EventsKeptBeforeARestartAreNotReplayed() {
        QVERIFY(StartDaemonWithASlowAgent());
        QVERIFY(ConnectApi());
        // The restart must ask B while A is out, and nothing tells the window
        // when it has: the delay is what keeps A out. It is long enough for
        // the events and the crash, which take milliseconds, and each of the
        // two lists waits it out.
        QVERIFY(SetSlowListing(3000));
        QSignalSpy reset(&Api_->Sessions(), &api::SessionService::Reset);
        QSignalSpy inserted(&Api_->Sessions(),
                            &api::SessionService::RowInserted);
        // A, which waits for the slow agent.
        const QFuture<rpc::Result> refreshed = Api_->Sessions().Refresh();
        QVERIFY(Notify({StateChanged(Row("s3", "working"))}));
        QTRY_COMPARE_WITH_TIMEOUT(inserted.size(), 1, TIMEOUT_MS);
        QVERIFY(!refreshed.isFinished());
        // The restart asks B while A is out.
        const QFuture<rpc::Result> crashed =
            Api_->Commands().Run("fixture.crash");
        QVERIFY(WaitFor(crashed));
        QTRY_COMPARE_WITH_TIMEOUT(reset.size(), 2, TIMEOUT_MS);
        QCOMPARE(Api_->Sessions().IndexOf(AGENT, "s3"), -1);
    }

    // The answer still lists s1, which the fast agent has said is gone.
    void ARemovalDuringARefreshSurvivesAnOlderAnswer() {
        QVERIFY(StartDaemonWithASlowAgent());
        QVERIFY(ConnectApi());
        QVERIFY(Api_->Sessions().IndexOf(AGENT, "s1") >= 0);
        QVERIFY(HoldSlowListing());
        QSignalSpy reset(&Api_->Sessions(), &api::SessionService::Reset);
        QSignalSpy removed(&Api_->Sessions(), &api::SessionService::RowRemoved);
        const QFuture<rpc::Result> refreshed = Api_->Sessions().Refresh();
        QVERIFY(Notify({Removed("s1")}));
        QTRY_COMPARE_WITH_TIMEOUT(removed.size(), 1, TIMEOUT_MS);
        QVERIFY(!refreshed.isFinished());
        QVERIFY(ReleaseSlowListing());
        QVERIFY(WaitFor(refreshed));
        QVERIFY2(refreshed.result(), qPrintable(Why(refreshed.result())));
        QCOMPARE(reset.size(), 1);
        // The answer brought s1 back, and the kept removal took it away.
        QCOMPARE(removed.size(), 2);
        QCOMPARE(Api_->Sessions().IndexOf(AGENT, "s1"), -1);
        QCOMPARE(Api_->Sessions().Rows().size(), 1);
        QCOMPARE(Api_->Sessions().IndexOf(AGENT, "s2"), 0);
    }

    // A session new to the list that answers, seen and then gone meanwhile:
    // the answer does not have it, and the events must leave no row.
    void AChangeThenARemovalDuringARefreshLeaveNoRow() {
        QVERIFY(StartDaemonWithASlowAgent());
        QVERIFY(ConnectApi());
        QVERIFY(HoldSlowListing());
        QSignalSpy inserted(&Api_->Sessions(),
                            &api::SessionService::RowInserted);
        QSignalSpy removed(&Api_->Sessions(), &api::SessionService::RowRemoved);
        const QFuture<rpc::Result> refreshed = Api_->Sessions().Refresh();
        QVERIFY(Notify({StateChanged(Row("s3", "working")), Removed("s3")}));
        QTRY_COMPARE_WITH_TIMEOUT(removed.size(), 1, TIMEOUT_MS);
        QCOMPARE(inserted.size(), 1);
        QVERIFY(!refreshed.isFinished());
        QVERIFY(ReleaseSlowListing());
        QVERIFY(WaitFor(refreshed));
        QVERIFY2(refreshed.result(), qPrintable(Why(refreshed.result())));
        // Replayed after the answer: inserted and removed again, in order.
        QCOMPARE(inserted.size(), 2);
        QCOMPARE(removed.size(), 2);
        QCOMPARE(Api_->Sessions().IndexOf(AGENT, "s3"), -1);
        QCOMPARE(Api_->Sessions().Rows().size(), 2);
    }

    void AnAdapterThatCrashedIsUnavailable() {
        QVERIFY(StartDaemon());
        QVERIFY(SetAnswers(
            {{"agent.listSessions", QJsonObject{{AGENT, "crash"}}}}));
        Api_ = std::make_unique<api::Api>();
        QSignalSpy reset(&Api_->Sessions(), &api::SessionService::Reset);
        Api_->Connect(Daemon_->SocketPath());
        QVERIFY(reset.wait(TIMEOUT_MS));
        QVERIFY(Api_->Sessions().IsLoaded());
        QVERIFY(Api_->Sessions().Rows().isEmpty());
        const QJsonArray unavailable = Api_->Sessions().Unavailable();
        QCOMPARE(unavailable.size(), 1);
        QCOMPARE(unavailable[0].toObject().value("agent").toString(), AGENT);
        QVERIFY(
            !unavailable[0].toObject().value("reason").toString().isEmpty());
    }

    void RefreshReplacesTheRowsAndResets() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        QVERIFY(SetAnswers(Answers({Row("s9", "done"), Row("s1", "failed")})));
        QSignalSpy reset(&Api_->Sessions(), &api::SessionService::Reset);
        QThread* finishedIn = nullptr;
        const QFuture<rpc::Result> refreshed = Api_->Sessions().Refresh().then(
            this, [&finishedIn](rpc::Result answer) {
                finishedIn = QThread::currentThread();
                return answer;
            });
        QVERIFY(WaitFor(refreshed));
        QVERIFY2(refreshed.result(), qPrintable(Why(refreshed.result())));
        QCOMPARE(finishedIn, QThread::currentThread());
        QCOMPARE(reset.size(), 1);
        // Gone rows are gone, and the order is the new answer's.
        QCOMPARE(Api_->Sessions().Rows(),
                 (QList<QJsonObject>{Row("s9", "done"), Row("s1", "failed")}));
        QCOMPARE(Api_->Sessions().IndexOf(AGENT, "s2"), -1);
        QCOMPARE(Api_->Sessions().IndexOf(AGENT, "s1"), 1);
        // The answer is roostd's, as it came.
        QCOMPARE(refreshed.result()->toObject().value("sessions").toArray(),
                 (QJsonArray{Row("s9", "done"), Row("s1", "failed")}));
    }

    void TheDispatcherListAlwaysAsksRoostdAndRefreshesTheCache() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        QVERIFY(SetAnswers(Answers({Row("s7", "idle")})));
        QSignalSpy reset(&Api_->Sessions(), &api::SessionService::Reset);
        const QFuture<rpc::Result> listed =
            Api_->Dispatch().Call("sessions.list", {}, this);
        QVERIFY(WaitFor(listed));
        QVERIFY2(listed.result(), qPrintable(Why(listed.result())));
        QCOMPARE(listed.result()->toObject().value("sessions").toArray(),
                 (QJsonArray{Row("s7", "idle")}));
        QCOMPARE(reset.size(), 1);
        QCOMPARE(Api_->Sessions().Rows(),
                 (QList<QJsonObject>{Row("s7", "idle")}));
    }

    void ALostConnectionDropsTheRows() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        QSignalSpy reset(&Api_->Sessions(), &api::SessionService::Reset);
        QSignalSpy disconnected(Api_.get(), &api::Api::Disconnected);
        Daemon_->Stop();
        QVERIFY(disconnected.wait(TIMEOUT_MS));
        QTRY_COMPARE_WITH_TIMEOUT(reset.size(), 1, TIMEOUT_MS);
        QVERIFY(!Api_->Sessions().IsLoaded());
        QVERIFY(Api_->Sessions().Rows().isEmpty());
        QCOMPARE(Api_->Sessions().IndexOf(AGENT, "s1"), -1);
        // A refresh now fails as the connection does, and keeps nothing.
        const QFuture<rpc::Result> refreshed = Api_->Sessions().Refresh();
        QVERIFY(WaitFor(refreshed));
        QVERIFY(!refreshed.result());
        QCOMPARE(refreshed.result().error().Code, CONNECTION_LOST);
        QVERIFY(Api_->Sessions().Rows().isEmpty());
        QVERIFY(!Api_->Sessions().IsLoaded());
    }

    void AnEventWithoutAnAgentOrAnIdIsIgnored() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        QSignalSpy changed(&Api_->Sessions(), &api::SessionService::RowChanged);
        QSignalSpy inserted(&Api_->Sessions(),
                            &api::SessionService::RowInserted);
        QTest::ignoreMessage(QtWarningMsg,
                             "session.stateChanged without an agent or an id");
        // roostd takes only a well-formed session, so the bus is used to say
        // what roostd would never send.
        const QFuture<rpc::Result> published = Api_->Bus().Publish(
            "session.stateChanged", QJsonObject{{"state", "idle"}});
        QVERIFY(WaitFor(published));
        // The events come in order: once a good one is seen, the bad one is
        // behind us, and it changed nothing.
        const QJsonObject done = Row("s2", "done", "finished");
        QVERIFY(Notify({StateChanged(done)}));
        QTRY_COMPARE_WITH_TIMEOUT(Api_->Sessions().Rows().value(1), done,
                                  TIMEOUT_MS);
        QCOMPARE(changed.size(), 1);
        QVERIFY(inserted.isEmpty());
        QCOMPARE(Api_->Sessions().Rows().size(), 2);
    }

    void ARemovalWithoutAnAgentOrAnIdIsIgnored() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        QSignalSpy removed(&Api_->Sessions(), &api::SessionService::RowRemoved);
        // Other session.* events are not for the cache, and not worth a
        // warning.
        QTest::failOnWarning();
        QFuture<rpc::Result> published =
            Api_->Bus().Publish("session.other", QJsonObject{{"id", "s1"}});
        QVERIFY(WaitFor(published));
        for (int i = 0; i < 2; ++i) {
            QTest::ignoreMessage(QtWarningMsg,
                                 "session.removed without an agent or an id");
        }
        published =
            Api_->Bus().Publish("session.removed", QJsonObject{{"id", "s1"}});
        QVERIFY(WaitFor(published));
        published = Api_->Bus().Publish("session.removed",
                                        QJsonObject{{"agent", AGENT}});
        QVERIFY(WaitFor(published));
        // The events come in order: once a good one is seen, the bad ones are
        // behind us, and they removed nothing.
        const QJsonObject done = Row("s2", "done", "finished");
        QVERIFY(Notify({StateChanged(done)}));
        QTRY_COMPARE_WITH_TIMEOUT(Api_->Sessions().Rows().value(1), done,
                                  TIMEOUT_MS);
        QVERIFY(removed.isEmpty());
        QCOMPARE(Api_->Sessions().Rows().size(), 2);
    }

    void ACallFromAnotherThreadIsRefused() {
#ifdef NDEBUG
        QSKIP("the check is compiled out of release builds");
#endif
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        const auto previous = api::SetCheckHandler(
            [](const char* /*message*/) { /* returns: the call is refused */ });
        QJsonArray seen;
        std::unique_ptr<QThread> thread(QThread::create([&] {
            auto& sessions = Api_->Sessions();
            sessions.Start();
            seen.append(sessions.IsLoaded());
            seen.append(sessions.Rows().size());
            seen.append(sessions.Unavailable().size());
            seen.append(sessions.IndexOf(AGENT, "s1"));
            const QFuture<rpc::Result> refreshed = sessions.Refresh();
            seen.append(refreshed.result().has_value());
        }));
        thread->start();
        QVERIFY(thread->wait(10000));
        api::SetCheckHandler(previous);
        // Each refused: false, nothing, nothing, -1, and an error.
        QCOMPARE(seen, (QJsonArray{false, 0, 0, -1, false}));
        QCOMPARE(Api_->Sessions().Rows().size(), 2);
    }
};

QTEST_GUILESS_MAIN(ApiSessionsTest)
#include "api_sessions_test.moc"
