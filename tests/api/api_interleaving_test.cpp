// What the window's API does when calls and connections overlap: a Refresh of
// the sessions in flight when the connection is lost, two Refreshes out at
// once, two Gets of one document at once, and the reconnect wait that a
// successful hello takes back to its first value. The sessions and the wait,
// against a daemon of the test's own, which answers when the test says so; the
// store, against the real roostd.

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <memory>
#include <vector>

#include "api/api.hpp"
#include "api_test_support.hpp"
#include "core/frame.hpp"
#include "core/protocol.hpp"
#include "core/version.hpp"
#include "helpers/roostd_process.hpp"

using namespace roost;
using namespace roost::api_test;

namespace {

constexpr int CONNECTION_LOST =
    static_cast<int>(protocol::EErrorCode::ConnectionLost);

// The wait grows 40, 80, 160, 320, 640 ms: a loss retried after the grown wait
// is far from one retried after the first.
constexpr std::chrono::milliseconds FIRST_DELAY{40};
constexpr std::chrono::milliseconds MAX_DELAY{640};
constexpr int ATTEMPTS_TO_GROW = 5;

api::ApiOptions Quick() {
    return {.FirstReconnectDelay = FIRST_DELAY, .MaxReconnectDelay = MAX_DELAY};
}

const QString AGENT = QStringLiteral("scripted-agent");

QJsonObject Row(const QString& id, const QString& state) {
    return {{"agent", AGENT},
            {"id", id},
            {"title", ""},
            {"cwd", ""},
            {"state", state}};
}

QJsonObject Doc(int pins) { return {{"pins", pins}}; }

QJsonObject Wrapped(const QJsonObject& document) {
    return {{"document", document}};
}

// The ids of the rows, in order, with the state of each: "s2:done".
QStringList Seen(const QList<QJsonObject>& rows) {
    QStringList seen;
    for (const QJsonObject& row : rows) {
        seen.append(row.value("id").toString() + ":" +
                    row.value("state").toString());
    }
    return seen;
}

// A roostd that is not one: it answers hello and bus.subscribe, keeps every
// sessions.list unanswered until the test answers it, and can send a bus event
// to the sessions' subscription or drop its connection.
class ScriptedDaemon : public QObject {
public:
    bool Listen(const QString& path) {
        connect(&Server_, &QLocalServer::newConnection, this, [this] {
            Arrived_.push_back(Clock_.elapsed());
            Socket_ = Server_.nextPendingConnection();
            Decoder_ = FrameDecoder();
            Lists_.clear();
            connect(Socket_, &QLocalSocket::readyRead, this, [this] {
                Decoder_.Feed(Socket_->readAll());
                while (const auto frame = Decoder_.Next()) {
                    OnRequest(QJsonDocument::fromJson(frame->Body).object());
                }
            });
            if (DropAtOnce) {
                Socket_->abort();
            }
        });
        Clock_.start();
        return Server_.listen(path);
    }

    // The connection goes, the daemon stays.
    void Drop() {
        DroppedAt_ = Clock_.elapsed();
        if (Socket_ != nullptr) {
            Socket_->abort();
        }
    }

    // The sessions.list requests of the present connection, in the order they
    // came.
    int Lists() const { return static_cast<int>(Lists_.size()); }

    // Answers the index-th list of the connection with these sessions.
    void AnswerList(int index, const QJsonArray& sessions) {
        Send({{"jsonrpc", "2.0"},
              {"id", Lists_.at(static_cast<std::size_t>(index))},
              {"result", QJsonObject{{"sessions", sessions},
                                     {"unavailable", QJsonArray{}}}}});
    }

    // An event of the bus, as the sessions' subscription gets it.
    void Event(const QString& name, const QJsonObject& payload) {
        Send({{"jsonrpc", "2.0"},
              {"method", "bus.event"},
              {"params", QJsonObject{{"subscription", SessionsSubscription_},
                                     {"name", name},
                                     {"payload", payload}}}});
    }

    // When each connection came, and when the last was dropped, in ms since
    // Listen.
    const std::vector<qint64>& Arrived() const { return Arrived_; }
    qint64 DroppedAt() const { return DroppedAt_; }

    bool DropAtOnce = false;

private:
    void Send(const QJsonObject& message) {
        Socket_->write(*EncodeFrame(
            EFrameKind::Rpc,
            QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }

    void Reply(const QJsonValue& id, const QJsonObject& result) {
        Send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
    }

    void OnRequest(const QJsonObject& request) {
        const QString method = request.value("method").toString();
        const QJsonValue id = request.value("id");
        if (method == "hello") {
            Reply(id, {{"roostd", QString::fromStdString(VersionString())}});
        } else if (method == "bus.subscribe") {
            ++Subscriptions_;
            if (request.value("params").toObject().value("pattern") ==
                QJsonValue("session.*")) {
                SessionsSubscription_ = Subscriptions_;
            }
            Reply(id, {{"subscription", Subscriptions_}});
        } else if (method == "sessions.list") {
            Lists_.push_back(id);
        }
    }

    QLocalServer Server_;
    QLocalSocket* Socket_ = nullptr;
    FrameDecoder Decoder_;
    QElapsedTimer Clock_;
    std::vector<qint64> Arrived_;
    qint64 DroppedAt_ = 0;
    std::vector<QJsonValue> Lists_;
    int Subscriptions_ = 0;
    int SessionsSubscription_ = 0;
};

}  // namespace

class ApiInterleavingTest : public QObject {
    Q_OBJECT

private:
    // The window is connected to the scripted daemon, and the list that the
    // connection asked for is answered with the sessions s1 (idle) and s2
    // (working).
    bool ConnectApi() {
        if (!Dir_->isValid() || !Daemon_->Listen(Dir_->filePath("fake.sock"))) {
            return false;
        }
        Api_ = std::make_unique<api::Api>(Quick());
        Connected_ =
            std::make_unique<QSignalSpy>(Api_.get(), &api::Api::Connected);
        Api_->Connect(Dir_->filePath("fake.sock"));
        if (!QTest::qWaitFor([this] { return Daemon_->Lists() == 1; },
                             TIMEOUT_MS)) {
            return false;
        }
        Daemon_->AnswerList(0, {Row("s1", "idle"), Row("s2", "working")});
        return QTest::qWaitFor([this] { return Api_->Sessions().IsLoaded(); },
                               TIMEOUT_MS);
    }

    // Two lists are asked for while the first is out, and these events come
    // while both are: s2 is done, s1 is gone. Returns the futures of the
    // lists. The events are told as rows, as the first list is in.
    bool TwoRefreshesWithEvents(QFuture<rpc::Result>& older,
                                QFuture<rpc::Result>& newer) {
        if (!ConnectApi()) {
            return false;
        }
        older = Api_->Sessions().Refresh();
        newer = Api_->Sessions().Refresh();
        if (!QTest::qWaitFor([this] { return Daemon_->Lists() == 3; },
                             TIMEOUT_MS)) {
            return false;
        }
        QSignalSpy changed(&Api_->Sessions(), &api::SessionService::RowChanged);
        QSignalSpy removed(&Api_->Sessions(), &api::SessionService::RowRemoved);
        Daemon_->Event("session.stateChanged", Row("s2", "done"));
        Daemon_->Event("session.removed", Row("s1", ""));
        return QTest::qWaitFor(
            [&] { return changed.size() == 1 && removed.size() == 1; },
            TIMEOUT_MS);
    }

    std::unique_ptr<ScriptedDaemon> Daemon_;
    std::unique_ptr<QTemporaryDir> Dir_;
    std::unique_ptr<api::Api> Api_;
    std::unique_ptr<QSignalSpy> Connected_;

    // The store's side, against the real roostd.
    bool StartRoostd() {
        return Roostd_->IsValid() &&
               Roostd_->AddFixturePlugin("fixture", FIXTURE_BACKEND_PATH) &&
               Roostd_->Start(ROOSTD_PATH);
    }

    bool WriteBehindRoostd(const QString& plugin, const QJsonObject& document) {
        QFile file(Roostd_->ConfigPath() + "/" + plugin + ".json");
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               file.write(QJsonDocument(document).toJson()) > 0;
    }

    std::unique_ptr<roost::testing::RoostdProcess> Roostd_;

private slots:
    void init() {
        Roostd_ = std::make_unique<roost::testing::RoostdProcess>();
        Daemon_ = std::make_unique<ScriptedDaemon>();
        Dir_ = std::make_unique<QTemporaryDir>();
    }

    void cleanup() {
        Connected_.reset();
        Api_.reset();
        Daemon_.reset();
        Dir_.reset();
        Roostd_.reset();
    }

    // The Refresh is out when the connection goes: it fails as the connection
    // does, the rows are gone, and the list that the new connection asks for
    // is the one that fills them.
    void ARefreshInFlightWhenTheConnectionIsLostLeavesOnlyTheNewList() {
        QVERIFY(ConnectApi());
        QCOMPARE(Seen(Api_->Sessions().Rows()),
                 (QStringList{"s1:idle", "s2:working"}));
        QSignalSpy reset(&Api_->Sessions(), &api::SessionService::Reset);
        const QFuture<rpc::Result> refreshed = Api_->Sessions().Refresh();
        QTRY_COMPARE_WITH_TIMEOUT(Daemon_->Lists(), 2, TIMEOUT_MS);

        Daemon_->Drop();
        QVERIFY(WaitFor(refreshed));
        QVERIFY(!refreshed.result());
        QCOMPARE(refreshed.result().error().Code, CONNECTION_LOST);
        // The failed future may be told before the loss is.
        QTRY_VERIFY_WITH_TIMEOUT(!Api_->Sessions().IsLoaded(), TIMEOUT_MS);
        QVERIFY(Api_->Sessions().Rows().isEmpty());
        QCOMPARE(Api_->Sessions().IndexOf(AGENT, "s1"), -1);
        QCOMPARE(reset.size(), 1);

        // Reconnected, and the rows stay dropped until the list of the new
        // connection comes.
        QTRY_COMPARE_WITH_TIMEOUT(Connected_->size(), 2, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Daemon_->Lists(), 1, TIMEOUT_MS);
        QVERIFY(Api_->Sessions().Rows().isEmpty());
        // An event that comes while the new list is out is kept for it, as it
        // is for any list.
        Daemon_->Event("session.stateChanged", Row("s3", "done"));
        Daemon_->AnswerList(0, {Row("s3", "working")});
        QTRY_VERIFY_WITH_TIMEOUT(Api_->Sessions().IsLoaded(), TIMEOUT_MS);
        QCOMPARE(Seen(Api_->Sessions().Rows()), (QStringList{"s3:done"}));
        QCOMPARE(Api_->Sessions().IndexOf(AGENT, "s1"), -1);
        QCOMPARE(reset.size(), 2);
    }

    // Two Refreshes are out; the later list answers first, and the earlier
    // one, which is older, is not put over it. The events that came while
    // they were out are on the rows once.
    void ALateAnswerOfAnEarlierRefreshDoesNotReplaceANewerList() {
        QFuture<rpc::Result> older;
        QFuture<rpc::Result> newer;
        QVERIFY(TwoRefreshesWithEvents(older, newer));
        QSignalSpy reset(&Api_->Sessions(), &api::SessionService::Reset);

        Daemon_->AnswerList(
            2, {Row("s1", "idle"), Row("s2", "working"), Row("s7", "idle")});
        QVERIFY(WaitFor(newer));
        QVERIFY(newer.result());
        QCOMPARE(reset.size(), 1);
        QCOMPARE(Seen(Api_->Sessions().Rows()),
                 (QStringList{"s2:done", "s7:idle"}));

        Daemon_->AnswerList(1, {Row("s1", "stale"), Row("s9", "idle")});
        QVERIFY(WaitFor(older));
        // The answer is the daemon's, whatever the cache did with it.
        QVERIFY(older.result());
        // The frames come in order: when the marker event is on the rows, the
        // answer before it has been handled.
        QSignalSpy inserted(&Api_->Sessions(),
                            &api::SessionService::RowInserted);
        Daemon_->Event("session.stateChanged", Row("s8", "idle"));
        QTRY_COMPARE_WITH_TIMEOUT(inserted.size(), 1, TIMEOUT_MS);
        QCOMPARE(reset.size(), 1);
        QCOMPARE(Seen(Api_->Sessions().Rows()),
                 (QStringList{"s2:done", "s7:idle", "s8:idle"}));
    }

    // Two Refreshes are out and answer in the order they were asked: the later
    // list is the one that stands, and the events that came while both were
    // out are on it, not lost with the first answer and not doubled.
    void TheLaterOfTwoRefreshesStandsWithTheEventsOfBoth() {
        QFuture<rpc::Result> older;
        QFuture<rpc::Result> newer;
        QVERIFY(TwoRefreshesWithEvents(older, newer));
        QSignalSpy reset(&Api_->Sessions(), &api::SessionService::Reset);

        Daemon_->AnswerList(1, {Row("s1", "stale"), Row("s9", "idle")});
        QVERIFY(WaitFor(older));
        QTRY_COMPARE_WITH_TIMEOUT(reset.size(), 1, TIMEOUT_MS);
        QCOMPARE(Seen(Api_->Sessions().Rows()),
                 (QStringList{"s9:idle", "s2:done"}));

        Daemon_->AnswerList(
            2, {Row("s1", "idle"), Row("s2", "working"), Row("s7", "idle")});
        QVERIFY(WaitFor(newer));
        QTRY_COMPARE_WITH_TIMEOUT(reset.size(), 2, TIMEOUT_MS);
        QCOMPARE(Seen(Api_->Sessions().Rows()),
                 (QStringList{"s2:done", "s7:idle"}));
        QCOMPARE(Api_->Sessions().IndexOf(AGENT, "s9"), -1);
        QCOMPARE(Api_->Sessions().IndexOf(AGENT, "s1"), -1);
    }

    // Both Gets miss the cache, as neither has answered; each asks roostd, and
    // both get the document. The cache is warm after, and the next Get does
    // not ask roostd: a document written behind its back is not seen.
    void TwoGetsAtOnceGetTheSameDocumentAndWarmTheCache() {
        QVERIFY(StartRoostd());
        api::Api api;
        QSignalSpy connected(&api, &api::Api::Connected);
        api.Connect(Roostd_->SocketPath());
        // The signal may be said inside Connect: look at the spy before
        // waiting.
        QVERIFY(!connected.isEmpty() || connected.wait(TIMEOUT_MS));
        // The store's directory is made by the first write.
        const QFuture<rpc::Result> saved = api.Store().Set("fixture", Doc(0));
        QVERIFY(WaitFor(saved));
        QVERIFY2(saved.result(), qPrintable(Why(saved.result())));
        QVERIFY(WriteBehindRoostd("fixture", Doc(1)));

        const QFuture<rpc::Result> first = api.Store().Get("fixture");
        const QFuture<rpc::Result> second = api.Store().Get("fixture");
        QVERIFY(!api.Store().IsWarm("fixture"));
        QVERIFY(WaitFor(first));
        QVERIFY(WaitFor(second));
        QVERIFY2(first.result(), qPrintable(Why(first.result())));
        QVERIFY2(second.result(), qPrintable(Why(second.result())));
        QCOMPARE(*first.result(), QJsonValue(Wrapped(Doc(1))));
        QCOMPARE(*second.result(), QJsonValue(Wrapped(Doc(1))));
        QVERIFY(api.Store().IsWarm("fixture"));

        QVERIFY(WriteBehindRoostd("fixture", Doc(2)));
        const QFuture<rpc::Result> third = api.Store().Get("fixture");
        QVERIFY(WaitFor(third));
        QVERIFY2(third.result(), qPrintable(Why(third.result())));
        QCOMPARE(*third.result(), QJsonValue(Wrapped(Doc(1))));
        // roostd does hold the newer one.
        const QFuture<rpc::Result> atRoostd =
            api.Io().Request("store.get", QJsonObject{{"plugin", "fixture"}});
        QVERIFY(WaitFor(atRoostd));
        QVERIFY2(atRoostd.result(), qPrintable(Why(atRoostd.result())));
        QCOMPARE(*atRoostd.result(), QJsonValue(Wrapped(Doc(2))));
    }

    // Failed attempts grow the wait to its cap. A hello that is answered
    // brings it back to the first, so that the next loss is retried after the
    // first wait and not the grown one.
    void ASuccessfulHelloTakesTheWaitBackToTheFirst() {
        // The daemon and the directory are members, so that they outlive Api_.
        ScriptedDaemon& daemon = *Daemon_;
        daemon.DropAtOnce = true;
        QVERIFY(Dir_->isValid());
        QVERIFY(daemon.Listen(Dir_->filePath("fake.sock")));
        Api_ = std::make_unique<api::Api>(Quick());
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        Api_->Connect(Dir_->filePath("fake.sock"));
        QTRY_VERIFY_WITH_TIMEOUT(
            static_cast<int>(daemon.Arrived().size()) >= ATTEMPTS_TO_GROW,
            TIMEOUT_MS);

        // The wait is at its cap now; the next attempt is let in.
        daemon.DropAtOnce = false;
        QTRY_COMPARE_WITH_TIMEOUT(connected.size(), 1, TIMEOUT_MS);

        daemon.Drop();
        const std::size_t lost = daemon.Arrived().size();
        QTRY_VERIFY_WITH_TIMEOUT(daemon.Arrived().size() > lost, TIMEOUT_MS);
        const qint64 waited = daemon.Arrived().at(lost) - daemon.DroppedAt();
        // A timer never fires early. Without the reset the wait is the cap
        // (640 ms), with it the first (40 ms); the bound is the middle between
        // the cap and the 4th wait (320 ms), so that a loaded machine has
        // ~440 ms of slack.
        QVERIFY2(waited >= FIRST_DELAY.count() - 2,
                 qPrintable(QString("waited %1 ms").arg(waited)));
        QVERIFY2(waited < MAX_DELAY.count() * 3 / 4,
                 qPrintable(QString("waited %1 ms").arg(waited)));
        QTRY_COMPARE_WITH_TIMEOUT(connected.size(), 2, TIMEOUT_MS);
    }
};

QTEST_GUILESS_MAIN(ApiInterleavingTest)
#include "api_interleaving_test.moc"
