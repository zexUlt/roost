// The reconnect of the window's API: the retries after a lost connection or a
// failed attempt, the calls that were pending, the listeners of the bus that
// are subscribed again under their ids, the caches that are made anew. Against
// the real roostd, stopped and started again on the same directories; the
// counting of what is sent, the delays and the refusal of hello, against a
// daemon of the test's own.

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
#include "helpers/roostd_process.hpp"

using namespace roost;
using namespace roost::api_test;

namespace {

constexpr int CONNECTION_LOST =
    static_cast<int>(protocol::EErrorCode::ConnectionLost);

// Small delays, so that a reconnect takes tens of milliseconds.
constexpr std::chrono::milliseconds FIRST_DELAY{20};
constexpr std::chrono::milliseconds MAX_DELAY{80};
constexpr int SEVERAL_ATTEMPTS_MS = 3 * 80;

api::ApiOptions Quick() {
    return {.FirstReconnectDelay = FIRST_DELAY, .MaxReconnectDelay = MAX_DELAY};
}

QJsonObject Doc(int pins) { return {{"pins", pins}}; }

QJsonObject Wrapped(const QJsonObject& document) {
    return {{"document", document}};
}

// A roostd that is not one: it answers hello as told, records every request
// it gets and answers nothing else. It can drop its connection.
class FakeDaemon : public QObject {
public:
    bool Listen(const QString& path) {
        connect(&Server_, &QLocalServer::newConnection, this, [this] {
            Arrived_.push_back(Clock_.elapsed());
            Socket_ = Server_.nextPendingConnection();
            Decoder_ = FrameDecoder();
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
        if (Socket_ != nullptr) {
            Socket_->abort();
        }
    }

    int Count(const QString& method) const {
        int count = 0;
        for (const QString& seen : Methods_) {
            count += seen == method ? 1 : 0;
        }
        return count;
    }

    // When each connection came, in ms since Listen.
    const std::vector<qint64>& Arrived() const { return Arrived_; }

    bool Refuse = false;
    bool DropAtOnce = false;

private:
    void OnRequest(const QJsonObject& request) {
        const QString method = request.value("method").toString();
        Methods_.push_back(method);
        if (method != "hello") {
            return;
        }
        QJsonObject answer{{"jsonrpc", "2.0"}, {"id", request.value("id")}};
        if (Refuse) {
            const rpc::Error error = protocol::MakeError(
                protocol::EErrorCode::VersionMismatch, "roostd speaks 0");
            answer.insert("error", QJsonObject{{"code", error.Code},
                                               {"message", error.Message}});
        } else {
            answer.insert("result", QJsonObject{});
        }
        Socket_->write(
            *EncodeFrame(EFrameKind::Rpc,
                         QJsonDocument(answer).toJson(QJsonDocument::Compact)));
    }

    QLocalServer Server_;
    QLocalSocket* Socket_ = nullptr;
    FrameDecoder Decoder_;
    QElapsedTimer Clock_;
    std::vector<qint64> Arrived_;
    std::vector<QString> Methods_;
};

}  // namespace

class ApiReconnectTest : public QObject {
    Q_OBJECT

private:
    bool StartDaemon() {
        return Daemon_->IsValid() &&
               Daemon_->AddFixturePlugin("fixture", FIXTURE_BACKEND_PATH) &&
               Daemon_->Start(ROOSTD_PATH);
    }

    // Connects the window to the daemon, and the first lists are in.
    bool ConnectApi() {
        Api_ = std::make_unique<api::Api>(Quick());
        Connected_ =
            std::make_unique<QSignalSpy>(Api_.get(), &api::Api::Connected);
        Disconnected_ =
            std::make_unique<QSignalSpy>(Api_.get(), &api::Api::Disconnected);
        Api_->Connect(Daemon_->SocketPath());
        return Reconnected(1);
    }

    // The loss of the daemon, seen by the window.
    bool StopDaemon() {
        const auto before = Disconnected_->size();
        return Daemon_->Stop() &&
               QTest::qWaitFor([&] { return Disconnected_->size() > before; },
                               TIMEOUT_MS);
    }

    // Connected the given number of times, and the sessions listed again.
    bool Reconnected(int times) {
        return QTest::qWaitFor([&] { return Connected_->size() >= times; },
                               TIMEOUT_MS) &&
               QTest::qWaitFor([this] { return Api_->Sessions().IsLoaded(); },
                               TIMEOUT_MS);
    }

    // Stops the daemon and starts it again: the window is connected anew.
    bool Restart(int times) {
        return StopDaemon() && Daemon_->Start(ROOSTD_PATH) &&
               Reconnected(times);
    }

    static rpc::Result Wait(const QFuture<rpc::Result>& future) {
        if (!WaitFor(future)) {
            return std::unexpected(rpc::Error{.Message = "timed out"});
        }
        return future.result();
    }

    bool Publish(const QString& name, const QJsonValue& payload) {
        return Wait(Api_->Bus().Publish(name, payload)).has_value();
    }

    // How many subscriptions roostd holds for the connection. Takes them: it
    // unsubscribes every id it finds, so it ends a test.
    int SubscriptionsAtRoostd() {
        int found = 0;
        for (int id = 1; id <= 32; ++id) {
            const auto answer = Api_->Io().Request(
                "bus.unsubscribe", QJsonObject{{"subscription", id}});
            if (WaitFor(answer) && answer.result()) {
                ++found;
            }
        }
        return found;
    }

    std::unique_ptr<roost::testing::RoostdProcess> Daemon_;
    std::unique_ptr<api::Api> Api_;
    std::unique_ptr<QSignalSpy> Connected_;
    std::unique_ptr<QSignalSpy> Disconnected_;

private slots:
    void init() { Daemon_ = std::make_unique<roost::testing::RoostdProcess>(); }

    void cleanup() {
        Connected_.reset();
        Disconnected_.reset();
        Api_.reset();
        Daemon_.reset();
    }

    void ADaemonThatComesBackIsConnectedAgain() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());

        QVERIFY(StopDaemon());
        QVERIFY(!Api_->IsConnected());
        // Let several attempts fail: they are silent.
        QTest::qWait(SEVERAL_ATTEMPTS_MS);
        QCOMPARE(Disconnected_->size(), 1);

        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Reconnected(2));
        QVERIFY(Api_->IsConnected());
        QCOMPARE(Disconnected_->size(), 1);
        // The caches were fetched again.
        QTRY_VERIFY_WITH_TIMEOUT(Api_->Plugins().IsLoaded(), TIMEOUT_MS);
        QVERIFY(Api_->Sessions().IndexOf("fixture-agent", "s1") >= 0);
    }

    void AFirstAttemptThatFailsIsRetried() {
        QVERIFY(Daemon_->IsValid());
        QVERIFY(Daemon_->AddFixturePlugin("fixture", FIXTURE_BACKEND_PATH));
        Api_ = std::make_unique<api::Api>(Quick());
        Connected_ =
            std::make_unique<QSignalSpy>(Api_.get(), &api::Api::Connected);
        Disconnected_ =
            std::make_unique<QSignalSpy>(Api_.get(), &api::Api::Disconnected);
        QSignalSpy refused(Api_.get(), &api::Api::Refused);
        Api_->Connect(Daemon_->SocketPath());
        QVERIFY(Disconnected_->wait(TIMEOUT_MS));
        QTest::qWait(SEVERAL_ATTEMPTS_MS);
        QCOMPARE(Disconnected_->size(), 1);

        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Reconnected(1));
        QCOMPARE(Disconnected_->size(), 1);
        QVERIFY(refused.isEmpty());
    }

    void APendingCallFailsWithConnectionLost() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());

        // The backend never answers it. A request behind it on the same
        // socket is answered, so roostd has had the first by then.
        const QFuture<rpc::Result> hung =
            Api_->Commands().Run("fixture.hang", QJsonArray{});
        QVERIFY(Publish("probe", 1));
        QVERIFY(!hung.isFinished());

        QVERIFY(StopDaemon());
        QVERIFY(WaitFor(hung));
        QVERIFY(!hung.result());
        QCOMPARE(hung.result().error().Code, CONNECTION_LOST);
    }

    void APendingCallIsNotSentAgain() {
        FakeDaemon daemon;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(daemon.Listen(dir.filePath("fake.sock")));
        Api_ = std::make_unique<api::Api>(Quick());
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        Api_->Connect(dir.filePath("fake.sock"));
        // The signal may be said inside Connect: look at the spy before
        // waiting.
        QVERIFY(!connected.isEmpty() || connected.wait(TIMEOUT_MS));

        const QFuture<rpc::Result> run =
            Api_->Commands().Run("fixture.pid", QJsonArray{});
        QTRY_COMPARE_WITH_TIMEOUT(daemon.Count("commands.run"), 1, TIMEOUT_MS);

        daemon.Drop();
        QVERIFY(WaitFor(run));
        QCOMPARE(run.result().error().Code, CONNECTION_LOST);
        QTRY_COMPARE_WITH_TIMEOUT(connected.size(), 2, TIMEOUT_MS);
        // The caches ask again, the command does not.
        QTRY_COMPARE_WITH_TIMEOUT(daemon.Count("sessions.list"), 2, TIMEOUT_MS);
        QTest::qWait(SEVERAL_ATTEMPTS_MS);
        QCOMPARE(daemon.Count("commands.run"), 1);
    }

    void TheWaitDoublesUpToTheCap() {
        FakeDaemon daemon;
        daemon.DropAtOnce = true;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(daemon.Listen(dir.filePath("fake.sock")));
        Api_ = std::make_unique<api::Api>(Quick());
        QSignalSpy disconnected(Api_.get(), &api::Api::Disconnected);
        Api_->Connect(dir.filePath("fake.sock"));
        QTRY_VERIFY_WITH_TIMEOUT(daemon.Arrived().size() >= 6, TIMEOUT_MS);
        // The gaps are at least the delays: 20, 40, 80, 80, 80 ms. A timer
        // never fires early, so the bound holds on a slow machine too.
        const std::vector<qint64>& at = daemon.Arrived();
        const std::vector<qint64> least{20, 40, 80, 80, 80};
        for (std::size_t i = 0; i < least.size(); ++i) {
            QVERIFY2(
                at[i + 1] - at[i] >= least[i] - 2,
                qPrintable(
                    QString("gap %1 is %2 ms").arg(i).arg(at[i + 1] - at[i])));
        }
        // Only the first failure is told.
        QCOMPARE(disconnected.size(), 1);
    }

    void ARefusalStopsTheRetriesAndConnectStartsThem() {
        FakeDaemon daemon;
        daemon.Refuse = true;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(daemon.Listen(dir.filePath("fake.sock")));
        Api_ = std::make_unique<api::Api>(Quick());
        QSignalSpy refused(Api_.get(), &api::Api::Refused);
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        Api_->Connect(dir.filePath("fake.sock"));
        QVERIFY(!refused.isEmpty() || refused.wait(TIMEOUT_MS));
        QCOMPARE(qvariant_cast<rpc::Error>(refused.at(0).at(0)).Code,
                 static_cast<int>(protocol::EErrorCode::VersionMismatch));
        QTest::qWait(SEVERAL_ATTEMPTS_MS);
        QCOMPARE(daemon.Count("hello"), 1);
        QCOMPARE(refused.size(), 1);

        // A daemon of the right build: a new Connect tries again.
        daemon.Refuse = false;
        Api_->Connect(dir.filePath("fake.sock"));
        QVERIFY(!connected.isEmpty() || connected.wait(TIMEOUT_MS));
        QCOMPARE(daemon.Count("hello"), 2);
        QVERIFY(Api_->IsConnected());
    }

    // The Disconnected that follows the refusal is still on its way when the
    // slot calls Connect: it must not start a second attempt over the first.
    void AConnectFromTheRefusedSlotMakesOneAttempt() {
        FakeDaemon daemon;
        daemon.Refuse = true;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(daemon.Listen(dir.filePath("fake.sock")));
        Api_ = std::make_unique<api::Api>(Quick());
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        connect(Api_.get(), &api::Api::Refused, this, [&] {
            daemon.Refuse = false;
            Api_->Connect(dir.filePath("fake.sock"));
        });
        Api_->Connect(dir.filePath("fake.sock"));
        QVERIFY(!connected.isEmpty() || connected.wait(TIMEOUT_MS));
        QTest::qWait(SEVERAL_ATTEMPTS_MS);
        QCOMPARE(daemon.Count("hello"), 2);
        QCOMPARE(connected.size(), 1);
        QVERIFY(Api_->IsConnected());
        QCOMPARE(daemon.Count("bus.subscribe"), 1);
    }

    void ASecondConnectWhileTryingIsStillAWarning() {
        QVERIFY(Daemon_->IsValid());
        QVERIFY(Daemon_->AddFixturePlugin("fixture", FIXTURE_BACKEND_PATH));
        Api_ = std::make_unique<api::Api>(Quick());
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        Api_->Connect(Daemon_->SocketPath());
        QTest::ignoreMessage(
            QtWarningMsg,
            "Api::Connect is called once; the connection is left as it is");
        Api_->Connect(Daemon_->SocketPath());
        // The retries that were running reach the daemon once it is up, and
        // the second call has added none of its own.
        // Start runs the event loop, so Connected may come inside it.
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QTRY_VERIFY_WITH_TIMEOUT(!connected.isEmpty(), TIMEOUT_MS);
        QTest::qWait(SEVERAL_ATTEMPTS_MS);
        QCOMPARE(connected.size(), 1);
    }

    void ListenersHearAfterTheReconnectUnderTheirIds() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());

        QStringList before;
        QStringList during;
        const auto early = Api_->Bus().Subscribe(
            "t.before", this, [&](const QString& name, const QJsonValue&) {
                before.append(name);
            });
        QVERIFY(early);
        QVERIFY(Publish("t.before", 1));
        QTRY_COMPARE_WITH_TIMEOUT(before.size(), 1, TIMEOUT_MS);

        QVERIFY(StopDaemon());
        // While there is no connection a subscription is still made.
        const auto late = Api_->Bus().Subscribe(
            "t.during", this, [&](const QString& name, const QJsonValue&) {
                during.append(name);
            });
        QVERIFY(late);
        QVERIFY(*late != *early);

        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Reconnected(2));
        QVERIFY(Publish("t.before", 2));
        QVERIFY(Publish("t.during", 3));
        QTRY_COMPARE_WITH_TIMEOUT(before.size(), 2, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(during.size(), 1, TIMEOUT_MS);

        // The ids are the same ones: they still unsubscribe.
        Api_->Bus().Unsubscribe(*early);
        QVERIFY(Publish("t.before", 4));
        QVERIFY(Publish("t.during", 5));
        QTRY_COMPARE_WITH_TIMEOUT(during.size(), 2, TIMEOUT_MS);
        QCOMPARE(before.size(), 2);
    }

    void AnUnsubscribeWithoutAConnectionForgetsTheListener() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        int heard = 0;
        const auto id = Api_->Bus().Subscribe(
            "t.gone", this,
            [&](const QString&, const QJsonValue&) { ++heard; });
        QVERIFY(id);
        QVERIFY(StopDaemon());
        Api_->Bus().Unsubscribe(*id);
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Reconnected(2));
        QVERIFY(Publish("t.gone", 1));
        QTest::qWait(50);
        QCOMPARE(heard, 0);
        // Only the sessions cache is subscribed.
        QCOMPARE(SubscriptionsAtRoostd(), 1);
    }

    void ADocumentChangedWhileTheDaemonWasDownIsReadAnew() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        QVERIFY(Wait(Api_->Store().Set("fixture", Doc(1))));
        QTRY_VERIFY_WITH_TIMEOUT(
            Wait(Api_->Store().Get("fixture")).value_or(QJsonValue()) ==
                QJsonValue(Wrapped(Doc(1))),
            TIMEOUT_MS);
        QVERIFY(Api_->Store().IsWarm("fixture"));

        QVERIFY(StopDaemon());
        QFile file(Daemon_->ConfigPath() + "/fixture.json");
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.write(QJsonDocument(Doc(7)).toJson()) > 0);
        file.close();

        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QVERIFY(Reconnected(2));
        QVERIFY(!Api_->Store().IsWarm("fixture"));
        const rpc::Result got = Wait(Api_->Store().Get("fixture"));
        QVERIFY2(got, qPrintable(Why(got)));
        QCOMPARE(*got, QJsonValue(Wrapped(Doc(7))));
        QVERIFY(Api_->Store().IsWarm("fixture"));

        // And the document is kept fresh again.
        QSignalSpy changed(&Api_->Store(), &api::StoreService::Changed);
        QVERIFY(Wait(Api_->Store().Set("fixture", Doc(8))));
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, TIMEOUT_MS);
        const rpc::Result cached = Wait(Api_->Store().Get("fixture"));
        QCOMPARE(*cached, QJsonValue(Wrapped(Doc(8))));
    }

    void ServicesSubscribeOnceAfterManyReconnects() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        QVERIFY(Wait(Api_->Store().Get("fixture")));

        for (int round = 2; round <= 4; ++round) {
            QVERIFY(Restart(round));
        }
        QVERIFY(Wait(Api_->Store().Get("fixture")));

        // One listener each: an event changes a row, and a document, once.
        QSignalSpy rows(&Api_->Sessions(), &api::SessionService::RowChanged);
        QSignalSpy changed(&Api_->Store(), &api::StoreService::Changed);
        QVERIFY(Publish("session.stateChanged",
                        QJsonObject{{"agent", "fixture-agent"},
                                    {"id", "s1"},
                                    {"title", "t"},
                                    {"cwd", "/"},
                                    {"state", "working"}}));
        QVERIFY(Wait(Api_->Store().Set("fixture", Doc(2))));
        QTRY_COMPARE_WITH_TIMEOUT(rows.size(), 1, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, TIMEOUT_MS);
        QTest::qWait(100);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(changed.size(), 1);

        // At roostd: the sessions' pattern and the store's, one each.
        QCOMPARE(SubscriptionsAtRoostd(), 2);
    }
};

QTEST_GUILESS_MAIN(ApiReconnectTest)
#include "api_reconnect_test.moc"
