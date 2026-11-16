// The method dispatcher of the window against the real roostd, with a plugin
// whose backend is the fixture backend: every method answers, in the GUI
// thread; what is not exposed is MethodNotFound; the errors of roostd pass
// unchanged.

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <atomic>
#include <memory>

#include "api/api.hpp"
#include "api_test_support.hpp"
#include "core/protocol.hpp"
#include "helpers/roostd_process.hpp"

using namespace roost;
using namespace roost::api_test;

namespace {

constexpr int BACKEND_UNAVAILABLE =
    static_cast<int>(protocol::EErrorCode::BackendUnavailable);
constexpr int STORE_FAILED =
    static_cast<int>(protocol::EErrorCode::StoreFailed);
constexpr int CONNECTION_LOST =
    static_cast<int>(protocol::EErrorCode::ConnectionLost);
constexpr int METHOD_NOT_FOUND =
    static_cast<int>(rpc::EErrorCode::MethodNotFound);
constexpr int INVALID_PARAMS = static_cast<int>(rpc::EErrorCode::InvalidParams);

}  // namespace

class ApiDispatcherTest : public QObject {
    Q_OBJECT

private:
    bool StartDaemon() {
        return Daemon_->IsValid() &&
               Daemon_->AddFixturePlugin("fixture", FIXTURE_BACKEND_PATH) &&
               Daemon_->Start(ROOSTD_PATH);
    }

    bool ConnectApi() {
        Api_ = std::make_unique<api::Api>();
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        Api_->Connect(Daemon_->SocketPath());
        return connected.wait(TIMEOUT_MS);
    }

    // The thread in which the future finished is recorded by a continuation
    // without a context: it runs in the thread that finishes the future.
    rpc::Result Call(const QString& method, const QJsonValue& params,
                     QThread** finishedIn = nullptr) {
        std::atomic<QThread*> thread{nullptr};
        const QFuture<rpc::Result> future =
            Api_->Dispatch()
                .Call(method, params, this)
                .then([&thread](rpc::Result answer) {
                    thread = QThread::currentThread();
                    return answer;
                });
        if (!WaitFor(future)) {
            return std::unexpected(rpc::Error{.Message = "timed out"});
        }
        if (finishedIn != nullptr) {
            *finishedIn = thread;
        }
        return future.result();
    }

    std::unique_ptr<roost::testing::RoostdProcess> Daemon_;
    std::unique_ptr<api::Api> Api_;

private slots:
    void init() { Daemon_ = std::make_unique<roost::testing::RoostdProcess>(); }

    void cleanup() {
        Api_.reset();
        Daemon_.reset();
    }

    void MethodsAreTheExposedOnes() {
        Api_ = std::make_unique<api::Api>();
        QCOMPARE(Api_->Dispatch().Methods(),
                 (QStringList{"bus.publish", "commands.run", "plugins.list",
                              "sessions.list", "store.get", "store.set"}));
    }

    void EveryMethodRoundTripsInTheGuiThread() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        const QJsonObject document{{"pins", QJsonArray{"a"}}};
        // The params of each method; a method without a line fails the test,
        // so that a new one is not left unchecked.
        const QMap<QString, QJsonObject> params{
            {"bus.publish", {{"name", "x.y"}, {"payload", 1}}},
            {"commands.run",
             {{"command", "fixture.echo"}, {"args", QJsonArray{1, "two"}}}},
            {"plugins.list", {}},
            {"sessions.list", {}},
            {"store.get", {{"plugin", "fixture"}}},
            {"store.set", {{"plugin", "fixture"}, {"document", document}}},
        };
        const QStringList methods = Api_->Dispatch().Methods();
        QCOMPARE(QStringList(params.keyBegin(), params.keyEnd()), methods);
        for (const QString& method : methods) {
            QThread* finishedIn = nullptr;
            const rpc::Result answer =
                Call(method, params.value(method), &finishedIn);
            QVERIFY2(answer, qPrintable(method + ": " + Why(answer)));
            QCOMPARE(finishedIn, QThread::currentThread());
        }
        // And what they answered.
        QCOMPARE(*Call("commands.run", params["commands.run"]),
                 QJsonValue(QJsonArray{1, "two"}));
        QCOMPARE(*Call("store.get", params["store.get"]),
                 QJsonValue(QJsonObject{{"document", document}}));
        QCOMPARE(*Call("bus.publish", params["bus.publish"]), QJsonValue());
        const QJsonObject sessions = Call("sessions.list", {})->toObject();
        QCOMPARE(sessions.value("sessions").toArray().size(), 1);
        QCOMPARE(sessions.value("sessions")[0].toObject().value("id"),
                 QJsonValue("s1"));
        const QJsonObject plugins = Call("plugins.list", {})->toObject();
        QCOMPARE(plugins.value("plugins")[0].toObject().value("id"),
                 QJsonValue("fixture"));
    }

    void WhatIsNotExposedIsMethodNotFound() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        for (const char* method :
             {"hello", "session.open", "session.resize", "session.close",
              "keymap.get", "bus.subscribe", "bus.unsubscribe", "nothing",
              "rpc.anything", ""}) {
            QThread* finishedIn = nullptr;
            const rpc::Result answer = Call(method, QJsonObject{}, &finishedIn);
            QVERIFY2(!answer, method);
            QCOMPARE(answer.error().Code, METHOD_NOT_FOUND);
            QVERIFY2(answer.error().Message.contains(method), method);
        }
    }

    void RegisterRefusesTakenAndReservedNames() {
        Api_ = std::make_unique<api::Api>();
        const rpc::Handler handler = [](const rpc::Call&) {
            return rpc::Ready(QJsonValue(7));
        };
        QVERIFY(!Api_->Dispatch().Register("store.get", handler));
        QVERIFY(!Api_->Dispatch().Register("rpc.x", handler));
        QVERIFY(Api_->Dispatch().Register("test.seven", handler));
        QVERIFY(Api_->Dispatch().Methods().contains("test.seven"));
    }

    void ACommandReachesTheBackend() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        const rpc::Result first =
            Call("commands.run", QJsonObject{{"command", "fixture.pid"}});
        QVERIFY2(first, qPrintable(Why(first)));
        // A process id: the backend is another process than the test.
        QVERIFY(first->toInteger() > 0);
        QVERIFY(first->toInteger() != QCoreApplication::applicationPid());
        // The same through the service, with the args left out.
        const rpc::Result second = [&] {
            const auto future = Api_->Commands().Run("fixture.echo");
            return WaitFor(future) ? future.result()
                                   : rpc::Result(std::unexpected(rpc::Error{}));
        }();
        QVERIFY2(second, qPrintable(Why(second)));
        QVERIFY(second->isNull());
    }

    void ErrorsOfRoostdPassUnchanged() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        const auto code = [&](const QString& method,
                              const QJsonObject& params) {
            const rpc::Result answer = Call(method, params);
            return answer ? 0 : answer.error().Code;
        };
        QCOMPARE(code("commands.run", {{"command", "no.such"}}),
                 INVALID_PARAMS);
        QCOMPARE(code("store.get", {{"plugin", "no-such-plugin"}}),
                 INVALID_PARAMS);
        QCOMPARE(code("store.set", {{"plugin", "fixture"}, {"document", 3}}),
                 INVALID_PARAMS);
        QCOMPARE(code("bus.publish", {{"name", "not a name"}}), INVALID_PARAMS);
        // A bad shape is refused at once, with the same code.
        QCOMPARE(code("commands.run", {}), INVALID_PARAMS);
        QCOMPARE(code("bus.publish", {}), INVALID_PARAMS);
        // The message is roostd's own.
        const rpc::Result unknown =
            Call("commands.run", QJsonObject{{"command", "no.such"}});
        QCOMPARE(unknown.error().Message,
                 QString("no plugin has command no.such"));
        // The backend dies without answering.
        QCOMPARE(code("commands.run", {{"command", "fixture.crash"}}),
                 BACKEND_UNAVAILABLE);
    }

    void StoreFailureIsPassedOn() {
        // A file where roostd wants the directory of its documents.
        QVERIFY(Daemon_->IsValid());
        QFile blocker(Daemon_->ConfigPath());
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        const rpc::Result answer = Call(
            "store.set",
            QJsonObject{{"plugin", "fixture"}, {"document", QJsonObject{}}});
        QVERIFY(!answer);
        QCOMPARE(answer.error().Code, STORE_FAILED);
    }

    void PluginsListWaitsForTheFirstFetch() {
        QVERIFY(StartDaemon());
        Api_ = std::make_unique<api::Api>();
        QSignalSpy changed(&Api_->Plugins(), &api::PluginService::Changed);
        bool loadedAtConnect = true;
        QFuture<rpc::Result> early;
        connect(Api_.get(), &api::Api::Connected, this, [&] {
            loadedAtConnect = Api_->Plugins().IsLoaded();
            early = Api_->Dispatch().Call("plugins.list", {}, this);
        });
        Api_->Connect(Daemon_->SocketPath());
        QTRY_VERIFY_WITH_TIMEOUT(early.isValid(), TIMEOUT_MS);
        QVERIFY(WaitFor(early));
        QVERIFY(!loadedAtConnect);
        QVERIFY2(early.result(), qPrintable(Why(early.result())));
        QCOMPARE(changed.size(), 1);
        QVERIFY(Api_->Plugins().IsLoaded());
        QCOMPARE(QJsonValue(Api_->Plugins().Plugins()), *early.result());
        QVERIFY(early.result()->toObject().contains("problems"));
    }

    void PluginsListIsServedFromTheCache() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        QTRY_VERIFY_WITH_TIMEOUT(Api_->Plugins().IsLoaded(), TIMEOUT_MS);
        QSignalSpy disconnected(Api_.get(), &api::Api::Disconnected);
        Daemon_->Stop();
        QVERIFY(disconnected.wait(TIMEOUT_MS));
        // The daemon is gone and the answer is still there.
        const rpc::Result plugins = Call("plugins.list", {});
        QVERIFY2(plugins, qPrintable(Why(plugins)));
        QCOMPARE(plugins->toObject().value("plugins")[0].toObject().value("id"),
                 QJsonValue("fixture"));
        // What is not cached fails.
        const rpc::Result sessions = Call("sessions.list", {});
        QVERIFY(!sessions);
        QCOMPARE(sessions.error().Code, CONNECTION_LOST);
    }

    void PendingCallsFailWithConnectionLost() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        QSignalSpy disconnected(Api_.get(), &api::Api::Disconnected);
        // The backend never answers this one.
        const QFuture<rpc::Result> hanging = Api_->Dispatch().Call(
            "commands.run", QJsonObject{{"command", "fixture.hang"}}, this);
        QTest::qWait(200);
        QVERIFY(!hanging.isFinished());
        Daemon_->Stop();
        QVERIFY(WaitFor(hanging));
        QVERIFY(!hanging.result());
        QCOMPARE(hanging.result().error().Code, CONNECTION_LOST);
        QVERIFY(disconnected.size() > 0 || disconnected.wait(TIMEOUT_MS));
        QVERIFY(!Api_->IsConnected());
    }
};

QTEST_GUILESS_MAIN(ApiDispatcherTest)
#include "api_dispatcher_test.moc"
