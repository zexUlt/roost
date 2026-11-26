// The method dispatcher of the window against the real roostd, with a plugin
// whose backend is the fixture backend: every method answers, in the GUI
// thread; what is not exposed is MethodNotFound; the errors of roostd pass
// unchanged.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <atomic>
#include <memory>
#include <optional>
#include <utility>

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
        // The signal may be said inside Connect: look at the spy before
        // waiting.
        return !connected.isEmpty() || connected.wait(TIMEOUT_MS);
    }

    // The thread in which the future finished is recorded by a continuation
    // without a context: it runs in the thread that finishes the future.
    rpc::Result Call(const QString& method, const QJsonValue& params,
                     QThread** finishedIn = nullptr,
                     const QString& plugin = {}) {
        std::atomic<QThread*> thread{nullptr};
        const QFuture<rpc::Result> future =
            Api_->Dispatch()
                .Call(method, params, this, plugin)
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
                 (QStringList{"bus.publish", "commands.run", "keymap.get",
                              "keymap.resetCommand", "keymap.setCommand",
                              "plugins.list", "sessions.list", "store.get",
                              "store.patch", "store.set"}));
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
            {"keymap.get", {}},
            {"keymap.resetCommand", {{"command", "fixture.echo"}}},
            {"keymap.setCommand",
             {{"command", "fixture.echo"},
              {"entries", QJsonArray{QJsonObject{{"key", "Ctrl+Alt+Z"}}}}}},
            {"plugins.list", {}},
            {"sessions.list", {}},
            {"store.get", {{"plugin", "fixture"}}},
            {"store.patch",
             {{"plugin", "fixture"}, {"patch", QJsonObject{{"n", 1}}}}},
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
        // The cache of the store follows the daemon's own change events, not
        // the answer to a write: wait for the last write to reach it.
        QTRY_VERIFY2_WITH_TIMEOUT(
            Api_->Store().Cached("fixture") == std::optional(document),
            qPrintable(QJsonDocument(Api_->Store().Cached("fixture").value_or(
                                         QJsonObject{{"cached", false}}))
                           .toJson(QJsonDocument::Compact)),
            TIMEOUT_MS);
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
              "bus.subscribe", "bus.unsubscribe", "nothing", "rpc.anything",
              ""}) {
            QThread* finishedIn = nullptr;
            const rpc::Result answer = Call(method, QJsonObject{}, &finishedIn);
            QVERIFY2(!answer, method);
            QCOMPARE(answer.error().Code, METHOD_NOT_FOUND);
            QVERIFY2(answer.error().Message.contains(method), method);
        }
    }

    // The window's table, for its own platform, as roostd put it together: the
    // key of the file over the plugin's, in the form of the platform.
    void KeymapGetAnswersTheTableOfTheWindowsPlatform() {
        QVERIFY(Daemon_->IsValid());
        QVERIFY(QDir().mkpath(Daemon_->ConfigPath()));
        QFile file(Daemon_->ConfigPath() + "/keymap.json");
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QJsonArray entries{
            QJsonObject{{"key", "Ctrl+Shift+Y"},
                        {"mac", "Cmd+Shift+Y"},
                        {"command", "fixture.echo"},
                        {"args", QJsonArray{1, "two"}}},
            QJsonObject{{"key", "Ctrl+Nope"}, {"command", "fixture.echo"}},
        };
        QVERIFY(file.write(QJsonDocument(entries).toJson()) > 0);
        file.close();
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        const QString chord =
            HostPlatform() == EPlatform::Mac ? "Cmd+Shift+Y" : "Ctrl+Shift+Y";
        // From the cache once loaded, and asked of roostd before.
        for (const char* what : {"loaded", "cached"}) {
            if (QString(what) == "loaded") {
                QVERIFY(QTest::qWaitFor(
                    [&] { return Api_->Keymap().IsLoaded(); }, TIMEOUT_MS));
            }
            const rpc::Result answer = Call("keymap.get", QJsonObject{});
            QVERIFY2(answer, qPrintable(Why(answer)));
            const QJsonObject table = answer->toObject();
            QCOMPARE(table.value("bindings").toArray(),
                     (QJsonArray{QJsonObject{{"key", chord},
                                             {"command", "fixture.echo"},
                                             {"args", QJsonArray{1, "two"}},
                                             {"source", "user"}}}));
            QCOMPARE(table.value("problems").toArray().size(), 1);
            QVERIFY(table.value("problems")[0].toString().contains(
                "keymap.json entry 1 is ignored"));
        }
        // The other platform is answered by roostd, in the form of that
        // platform; a name that is no platform is refused.
        const bool onMac = HostPlatform() == EPlatform::Mac;
        const QString other = onMac ? "linux" : "mac";
        const rpc::Result theirs =
            Call("keymap.get", QJsonObject{{"platform", other}});
        QVERIFY2(theirs, qPrintable(Why(theirs)));
        QCOMPARE(
            theirs->toObject().value("bindings")[0].toObject().value("key"),
            QJsonValue(onMac ? "Ctrl+Shift+Y" : "Cmd+Shift+Y"));
        const rpc::Result refused =
            Call("keymap.get", QJsonObject{{"platform", "plan9"}});
        QVERIFY(!refused);
        QCOMPARE(refused.error().Code, INVALID_PARAMS);
        const QString own = onMac ? "mac" : "linux";
        QVERIFY(Call("keymap.get", QJsonObject{{"platform", own}}));
    }

    // The window sets any command; a plugin sets its own (`<id>.*`) and is
    // refused for another's, with the file untouched.
    void APluginSetsTheKeysOfItsOwnCommandsOnly() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        const QString file = Daemon_->ConfigPath() + "/keymap.json";
        const QJsonObject set{
            {"command", "fixture.echo"},
            {"entries", QJsonArray{QJsonObject{{"key", "Ctrl+Alt+Z"}}}}};
        const QJsonObject reset{{"command", "fixture.echo"}};

        // Another plugin: refused, for both methods, and nothing is written.
        for (const auto& [method, params] :
             {std::pair<QString, QJsonObject>{"keymap.setCommand", set},
              std::pair<QString, QJsonObject>{"keymap.resetCommand", reset}}) {
            const rpc::Result refused = Call(method, params, nullptr, "other");
            QVERIFY2(!refused, qPrintable(method));
            QCOMPARE(refused.error().Code, INVALID_PARAMS);
            QVERIFY2(refused.error().Message.contains("fixture.echo") &&
                         refused.error().Message.contains("other"),
                     qPrintable(refused.error().Message));
        }
        // A plugin whose id only begins with the other's is another plugin.
        QVERIFY(!Call("keymap.setCommand", set, nullptr, "fixtur"));
        QVERIFY(!Call("keymap.setCommand", set, nullptr, "fixture.echo"));
        QVERIFY(!QFileInfo::exists(file));

        // Its own command: passes, and the file is written.
        const rpc::Result own =
            Call("keymap.setCommand", set, nullptr, "fixture");
        QVERIFY2(own, qPrintable(Why(own)));
        QFile written(file);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(written.readAll()).array(),
                 (QJsonArray{QJsonObject{{"command", "fixture.echo"},
                                         {"key", "Ctrl+Alt+Z"}}}));
        written.close();
        const rpc::Result back =
            Call("keymap.resetCommand", reset, nullptr, "fixture");
        QVERIFY2(back, qPrintable(Why(back)));
        QVERIFY(written.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(written.readAll()).array(),
                 QJsonArray{});
        written.close();

        // The window, with no plugin, sets any command.
        const rpc::Result window = Call("keymap.setCommand", set);
        QVERIFY2(window, qPrintable(Why(window)));
        QVERIFY(written.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(written.readAll()).array().size(), 1);
    }

    // What is wrong with the params is InvalidParams and does not reach roostd.
    void KeymapWritesWithBadParamsAreInvalid() {
        QVERIFY(StartDaemon());
        QVERIFY(ConnectApi());
        const QJsonArray none;
        for (const QJsonObject& params : {
                 QJsonObject{{"entries", none}},
                 QJsonObject{{"command", 1}, {"entries", none}},
                 QJsonObject{{"command", "fixture.echo"}},
                 QJsonObject{{"command", "fixture.echo"}, {"entries", 1}},
                 QJsonObject{{"command", "fixture.echo"},
                             {"entries", none},
                             {"when", 3}},
             }) {
            const rpc::Result answer = Call("keymap.setCommand", params);
            QVERIFY2(!answer, qPrintable(QJsonDocument(params).toJson()));
            QCOMPARE(answer.error().Code, INVALID_PARAMS);
        }
        const rpc::Result noCommand =
            Call("keymap.resetCommand", QJsonObject{});
        QVERIFY(!noCommand);
        QCOMPARE(noCommand.error().Code, INVALID_PARAMS);
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
        QVERIFY(plugins->toObject().contains("problems"));
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
