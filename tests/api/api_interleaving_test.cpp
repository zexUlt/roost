// What the window's API does when calls and connections overlap: a Refresh of
// the sessions in flight when the connection is lost, two Refreshes out at
// once, two Gets of one document at once, an event that overtakes the answer
// naming its subscription, a plugins.list that waits for the first fetch, and
// the reconnect wait that a successful hello takes back to its first value.
// All against a scripted daemon of the test's own, which answers when the test
// says so.

#include <QElapsedTimer>
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
            Gets_.clear();
            Plugins_.clear();
            HeldSubscription_ = 0;
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
        EventOn(SessionsSubscription_, name, payload);
    }

    // An event of the bus, for any subscription number.
    void EventOn(qint64 subscription, const QString& name,
                 const QJsonValue& payload) {
        Send({{"jsonrpc", "2.0"},
              {"method", "bus.event"},
              {"params", QJsonObject{{"subscription", subscription},
                                     {"name", name},
                                     {"payload", payload}}}});
    }

    // The bus.subscribe of this pattern is not answered until AnswerHeld; its
    // subscription number is known at once.
    void Hold(const QString& pattern) { HeldPattern_ = pattern; }
    bool IsHeld() const { return HeldSubscription_ != 0; }
    int HeldSubscription() const { return HeldSubscription_; }
    void AnswerHeld() {
        Reply(HeldId_, {{"subscription", HeldSubscription_}});
        HeldSubscription_ = 0;
    }

    // The store.get requests of the present connection, in the order they
    // came; AnswerGet answers the index-th with this document.
    int Gets() const { return static_cast<int>(Gets_.size()); }
    void AnswerGet(int index, const QJsonObject& document) {
        Reply(Gets_.at(static_cast<std::size_t>(index)),
              {{"document", document}});
    }

    // The plugins.list requests of the present connection, in the order they
    // came; AnswerPlugins answers the index-th with these plugins.
    int PluginsLists() const { return static_cast<int>(Plugins_.size()); }
    void AnswerPlugins(int index, const QJsonArray& plugins) {
        Reply(Plugins_.at(static_cast<std::size_t>(index)),
              {{"plugins", plugins}, {"problems", QJsonArray{}}});
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
            const QString pattern =
                request.value("params").toObject().value("pattern").toString();
            if (pattern == "session.*") {
                SessionsSubscription_ = Subscriptions_;
            }
            if (!HeldPattern_.isEmpty() && pattern == HeldPattern_) {
                HeldId_ = id;
                HeldSubscription_ = Subscriptions_;
                return;
            }
            Reply(id, {{"subscription", Subscriptions_}});
        } else if (method == "sessions.list") {
            Lists_.push_back(id);
        } else if (method == "store.get") {
            Gets_.push_back(id);
        } else if (method == "plugins.list") {
            Plugins_.push_back(id);
        }
    }

    QLocalServer Server_;
    QLocalSocket* Socket_ = nullptr;
    FrameDecoder Decoder_;
    QElapsedTimer Clock_;
    std::vector<qint64> Arrived_;
    qint64 DroppedAt_ = 0;
    std::vector<QJsonValue> Lists_;
    std::vector<QJsonValue> Gets_;
    std::vector<QJsonValue> Plugins_;
    QString HeldPattern_;
    QJsonValue HeldId_;
    int HeldSubscription_ = 0;
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

private slots:
    void init() {
        Daemon_ = std::make_unique<ScriptedDaemon>();
        Dir_ = std::make_unique<QTemporaryDir>();
    }

    void cleanup() {
        Connected_.reset();
        Api_.reset();
        Daemon_.reset();
        Dir_.reset();
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
    // not ask roostd. The daemon holds the answers, so that the order does not
    // depend on how fast the IO thread is.
    void TwoGetsAtOnceGetTheSameDocumentAndWarmTheCache() {
        QVERIFY(ConnectApi());
        const QFuture<rpc::Result> first = Api_->Store().Get("fixture");
        const QFuture<rpc::Result> second = Api_->Store().Get("fixture");
        QTRY_COMPARE_WITH_TIMEOUT(Daemon_->Gets(), 2, TIMEOUT_MS);
        QVERIFY(!first.isFinished());
        QVERIFY(!second.isFinished());
        QVERIFY(!Api_->Store().IsWarm("fixture"));

        Daemon_->AnswerGet(0, Doc(1));
        Daemon_->AnswerGet(1, Doc(1));
        QVERIFY(WaitFor(first));
        QVERIFY(WaitFor(second));
        QVERIFY2(first.result(), qPrintable(Why(first.result())));
        QVERIFY2(second.result(), qPrintable(Why(second.result())));
        QCOMPARE(*first.result(), QJsonValue(Wrapped(Doc(1))));
        QCOMPARE(*second.result(), QJsonValue(Wrapped(Doc(1))));
        QVERIFY(Api_->Store().IsWarm("fixture"));

        const QFuture<rpc::Result> third = Api_->Store().Get("fixture");
        QVERIFY(WaitFor(third));
        QVERIFY2(third.result(), qPrintable(Why(third.result())));
        QCOMPARE(*third.result(), QJsonValue(Wrapped(Doc(1))));
        QCOMPARE(Daemon_->Gets(), 2);
    }

    // An event of a subscription arrives before the answer that names it: it
    // is kept and told when the answer comes. An event of a subscription
    // nobody has is dropped. The daemon sends the event first and answers
    // after, so that the order does not depend on how fast the IO thread is.
    void AnEventThatOvertakesTheAnswerIsNotLost() {
        QVERIFY(ConnectApi());
        Daemon_->Hold("x.a");
        // y.b is asked first and answered at once; x.a is the one held, so
        // its number follows.
        QList<QJsonValue> marker;
        const auto markerId = Api_->Bus().Subscribe(
            "y.b", this, [&marker](const QString&, const QJsonValue& v) {
                marker.append(v);
            });
        QList<QPair<QString, QJsonValue>> heard;
        const auto id = Api_->Bus().Subscribe(
            "x.a", this, [&heard](const QString& name, const QJsonValue& v) {
                heard.append({name, v});
            });
        QVERIFY(id);
        QTRY_VERIFY_WITH_TIMEOUT(Daemon_->IsHeld(), TIMEOUT_MS);
        const int subscription = Daemon_->HeldSubscription();
        const int markerSubscription = subscription - 1;

        Daemon_->EventOn(subscription, "x.a", "early");
        Daemon_->EventOn(subscription + 100, "x.a", "stranger");
        // The marker is on a subscription that is answered. Events of one
        // connection keep their order, so when it is heard, "early" has been
        // handled too, and has been held, not told.
        QVERIFY(markerId);
        Daemon_->EventOn(markerSubscription, "y.b", "marker");
        QTRY_COMPARE_WITH_TIMEOUT(marker.size(), 1, TIMEOUT_MS);
        QVERIFY(heard.isEmpty());
        Daemon_->AnswerHeld();
        // Frames come in order: "later" is told after the answer is handled.
        Daemon_->EventOn(subscription, "x.a", "later");
        QTRY_COMPARE_WITH_TIMEOUT(heard.size(), 2, TIMEOUT_MS);
        QCOMPARE(heard.at(0).second, QJsonValue("early"));
        QCOMPARE(heard.at(1).second, QJsonValue("later"));
    }

    // A plugins.list asked for before the first fetch has answered waits for
    // it. The daemon holds the fetch's answer, so that the cache is not loaded
    // when Connected is said, whatever the speed of the IO thread.
    void PluginsListWaitsForTheFirstFetch() {
        QVERIFY(Dir_->isValid());
        QVERIFY(Daemon_->Listen(Dir_->filePath("fake.sock")));
        Api_ = std::make_unique<api::Api>(Quick());
        QSignalSpy changed(&Api_->Plugins(), &api::PluginService::Changed);
        bool loadedAtConnect = true;
        bool asked = false;
        QFuture<rpc::Result> early;
        connect(Api_.get(), &api::Api::Connected, this, [&] {
            loadedAtConnect = Api_->Plugins().IsLoaded();
            early = Api_->Dispatch().Call("plugins.list", {}, this);
            asked = true;
        });
        Api_->Connect(Dir_->filePath("fake.sock"));
        QTRY_VERIFY_WITH_TIMEOUT(asked, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Daemon_->PluginsLists(), 1, TIMEOUT_MS);
        QVERIFY(!loadedAtConnect);
        QVERIFY(!early.isFinished());
        QVERIFY(!Api_->Plugins().IsLoaded());

        Daemon_->AnswerPlugins(0, {QJsonObject{{"id", "fixture"}}});
        QVERIFY(WaitFor(early));
        QVERIFY2(early.result(), qPrintable(Why(early.result())));
        QCOMPARE(changed.size(), 1);
        QVERIFY(Api_->Plugins().IsLoaded());
        QCOMPARE(QJsonValue(Api_->Plugins().Plugins()), *early.result());
        QCOMPARE(Daemon_->PluginsLists(), 1);
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
