// The singleton Roost of module Roost.Api (calls, store, bus, connected) as
// QML uses it, in a QQmlEngine over the real roostd with a plugin whose
// backend is the fixture backend: every answer comes to its callback as
// callback(error, result) in the GUI thread, and every method of the
// dispatcher is reachable by name.

#include <QJSEngine>
#include <QJSValue>
#include <QJSValueList>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <vector>

#include "api/api.hpp"
#include "api/api_test_support.hpp"
#include "core/protocol.hpp"
#include "helpers/roostd_process.hpp"
#include "qml_api/roost_singleton.hpp"

using namespace roost;
using namespace roost::api_test;

// The static plugin of module Roost.Api, linked into this executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)

namespace {

constexpr int BACKEND_UNAVAILABLE =
    static_cast<int>(protocol::EErrorCode::BackendUnavailable);
constexpr int CONNECTION_LOST =
    static_cast<int>(protocol::EErrorCode::ConnectionLost);
constexpr int METHOD_NOT_FOUND =
    static_cast<int>(rpc::EErrorCode::MethodNotFound);
constexpr int INVALID_PARAMS = static_cast<int>(rpc::EErrorCode::InvalidParams);

QJsonObject Doc(int pins) { return {{"pins", pins}}; }

// What the callbacks of the QML below leave in `got`: JSON lines of
// {tag, err, res}, with `err` null on success and `res` absent on failure.
const char* const QML = R"(
import QtQml
import Roost.Api

QtObject {
    id: root
    property var got: []
    property var owners: ({})

    function rec(tag, err, res) {
        got.push(JSON.stringify({tag: tag, err: err, res: res}))
        gotChanged()
    }
    function run(command, args) {
        Roost.commands.run(command, args, function (e, r) {
            probe.Hit(command)
            rec(command, e, r)
        })
    }
    function runWithoutCallback(command) { Roost.commands.run(command) }
    function runThrowing(command) {
        Roost.commands.run(command, undefined, function (e, r) {
            throw new Error("boom")
        })
    }
    function runHanging() {
        Roost.commands.run("fixture.hang", undefined, function (e, r) {
            probe.Hit("hang")
            rec("hang", e, r)
        })
    }
    function call(method, params) {
        Roost.call(method, params, function (e, r) { rec(method, e, r) })
    }
    function storeGet(plugin) {
        Roost.store.get(plugin, function (e, r) { rec("get", e, r) })
    }
    function storeSet(plugin, document) {
        Roost.store.set(plugin, document, function (e, r) { rec("set", e, r) })
    }
    // Whether the callback ran inside the call itself.
    function runsInside(method, params) {
        var hit = false
        Roost.call(method, params, function (e, r) {
            hit = true
            rec("later:" + method, e, r)
        })
        return hit
    }
    function getRunsInside(plugin) {
        var hit = false
        Roost.store.get(plugin, function (e, r) {
            hit = true
            rec("later:get", e, r)
        })
        return hit
    }
    function setConverted(plugin) {
        Roost.store.set(plugin, {
            keep: 1, gone: undefined, fn: function () {}, nothing: null,
            when: new Date(Date.UTC(2020, 0, 2, 3, 4, 5)),
            list: [undefined, function () {}, 2],
            nested: {gone: undefined, keep: "x"}
        }, function (e, r) { rec("set", e, r) })
    }
    function storeWatch() {
        Roost.store.changed.connect(function (plugin, document) {
            rec("changed", plugin, document)
        })
    }
    function makeOwner(name) {
        owners[name] = Qt.createQmlObject("import QtQml; QtObject {}", root)
    }
    function killOwner(name) { owners[name].destroy() }
    function subscribe(pattern, ownerName, tag) {
        return Roost.bus.subscribe(pattern, owners[ownerName],
            function (name, payload) {
                probe.Hit(tag)
                rec(tag, name, payload)
            })
    }
    function unsubscribe(id) { Roost.bus.unsubscribe(id) }
    function publish(name, payload) {
        Roost.bus.publish(name, payload, function (e, r) { rec("publish", e, r) })
    }
    function thrownBy(what) {
        try {
            if (what === "null owner") {
                Roost.bus.subscribe("x.y", null, function () {})
            } else if (what === "bad pattern") {
                Roost.bus.subscribe("x.*.y", root, function () {})
            } else if (what === "not a function") {
                Roost.bus.subscribe("x.y", root, 5)
            }
        } catch (e) {
            return (e instanceof TypeError) ? "TypeError" : String(e)
        }
        return "nothing"
    }
    function isConnected() { return Roost.connected }
}
)";

// What the callbacks call, to learn the thread they run in.
class Probe : public QObject {
    Q_OBJECT

public:
    Q_INVOKABLE void Hit(const QString& tag) {
        Tags.push_back(tag);
        if (QThread::currentThread() !=
            QCoreApplication::instance()->thread()) {
            ++OutsideGuiThread;
        }
    }
    void Reset() {
        Tags.clear();
        OutsideGuiThread = 0;
    }
    int Count(const QString& tag) const {
        return static_cast<int>(std::count(Tags.begin(), Tags.end(), tag));
    }

    std::vector<QString> Tags;
    int OutsideGuiThread = 0;
};

}  // namespace

class RoostCallsTest : public QObject {
    Q_OBJECT

private:
    bool StartDaemon() {
        return Daemon_->IsValid() &&
               Daemon_->AddFixturePlugin("fixture", FIXTURE_BACKEND_PATH) &&
               Daemon_->Start(ROOSTD_PATH);
    }

    std::unique_ptr<api::Api> ConnectApi() {
        auto api = std::make_unique<api::Api>();
        QSignalSpy connected(api.get(), &api::Api::Connected);
        api->Connect(Daemon_->SocketPath());
        return connected.wait(TIMEOUT_MS) ? std::move(api) : nullptr;
    }

    // The daemon, the Api, the engine and the QML object, ready to call.
    bool Setup() {
        if (!StartDaemon()) {
            return false;
        }
        Api_ = ConnectApi();
        return Api_ && SetupPage(true);
    }

    // The engine and the QML object, with the Api of the window or without.
    bool SetupPage(bool withApi) {
        Engine_ = std::make_unique<QQmlEngine>();
        Engine_->rootContext()->setContextProperty("probe", &Probe_);
        Singleton_ =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        if (Singleton_ == nullptr) {
            return false;
        }
        if (withApi) {
            Singleton_->setApi(*Api_);
        }
        QQmlComponent component(Engine_.get());
        component.setData(QML, QUrl());
        Page_.reset(component.create());
        if (!Page_) {
            qWarning("%s", qPrintable(component.errorString()));
            return false;
        }
        QJSEngine::setObjectOwnership(Page_.get(), QJSEngine::CppOwnership);
        return true;
    }

    // Calls a function of the QML object, with JSON values as arguments.
    QJSValue Invoke(const char* function,
                    std::initializer_list<QJsonValue> arguments = {}) {
        QJSValueList converted;
        for (const QJsonValue& argument : arguments) {
            converted << Engine_->toScriptValue(argument);
        }
        QJSValue page = Engine_->newQObject(Page_.get());
        return page.property(function).callWithInstance(page, converted);
    }

    // The records of the callbacks so far.
    QList<QJsonObject> Got() const {
        QList<QJsonObject> records;
        const QJSValue got = Engine_->newQObject(Page_.get()).property("got");
        const quint32 length = got.property("length").toUInt();
        for (quint32 i = 0; i < length; ++i) {
            records.append(
                QJsonDocument::fromJson(got.property(i).toString().toUtf8())
                    .object());
        }
        return records;
    }

    // The first record with the tag, once it has come.
    QJsonObject WaitForTag(const QString& tag) {
        QJsonObject found;
        const bool arrived = QTest::qWaitFor(
            [&] {
                for (const QJsonObject& record : Got()) {
                    if (record.value("tag").toString() == tag) {
                        found = record;
                        return true;
                    }
                }
                return false;
            },
            TIMEOUT_MS);
        return arrived ? found : QJsonObject{{"timedOut", true}};
    }

    int CountTag(const QString& tag) const {
        int count = 0;
        for (const QJsonObject& record : Got()) {
            count += record.value("tag").toString() == tag ? 1 : 0;
        }
        return count;
    }

    static rpc::Result Wait(const QFuture<rpc::Result>& future) {
        if (!WaitFor(future)) {
            return std::unexpected(rpc::Error{.Message = "timed out"});
        }
        return future.result();
    }

    std::unique_ptr<roost::testing::RoostdProcess> Daemon_;
    std::unique_ptr<api::Api> Api_;
    Probe Probe_;
    std::unique_ptr<QQmlEngine> Engine_;
    RoostSingleton* Singleton_ = nullptr;
    std::unique_ptr<QObject> Page_;

private slots:
    void init() {
        Probe_.Reset();
        Daemon_ = std::make_unique<roost::testing::RoostdProcess>();
    }

    void cleanup() {
        // The engine goes first, as in main(): the Api outlives it.
        Page_.reset();
        Engine_.reset();
        Singleton_ = nullptr;
        Api_.reset();
        Daemon_.reset();
    }

    void ARunAnswersItsCallbackInTheGuiThread() {
        QVERIFY(Setup());
        Invoke("run", {"fixture.echo", QJsonObject{{"a", QJsonArray{1, "x"}}}});
        const QJsonObject record = WaitForTag("fixture.echo");
        QVERIFY2(!record.contains("timedOut"), "no answer");
        QVERIFY(record.contains("err"));
        QVERIFY(record.value("err").isNull());
        QCOMPARE(record.value("res"),
                 QJsonValue(QJsonObject{{"a", QJsonArray{1, "x"}}}));
        QCOMPARE(Probe_.Count("fixture.echo"), 1);
        QCOMPARE(Probe_.OutsideGuiThread, 0);
        // Absent args come back as null.
        Invoke("run", {"fixture.echo"});
        QTRY_COMPARE_WITH_TIMEOUT(CountTag("fixture.echo"), 2, TIMEOUT_MS);
    }

    void AnUnknownCommandFailsWithRoostdsError() {
        QVERIFY(Setup());
        Invoke("run", {"no.such"});
        const QJsonObject record = WaitForTag("no.such");
        QVERIFY2(!record.contains("timedOut"), "no answer");
        const QJsonObject error = record.value("err").toObject();
        QCOMPARE(error.value("code").toInt(), INVALID_PARAMS);
        QCOMPARE(error.value("message").toString(),
                 QString("no plugin has command no.such"));
        QVERIFY(!record.contains("res"));
    }

    void ACommandWhoseBackendIsGoneFailsWithBackendUnavailable() {
        QVERIFY(Setup());
        Invoke("run", {"fixture.crash"});
        const QJsonObject record = WaitForTag("fixture.crash");
        QVERIFY2(!record.contains("timedOut"), "no answer");
        QCOMPARE(record.value("err").toObject().value("code").toInt(),
                 BACKEND_UNAVAILABLE);
    }

    void AFailureWithoutACallbackIsLogged() {
        QVERIFY(Setup());
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression("roost: commands.run failed: no plugin has "
                               "command no.such \\(-32602\\)"));
        Invoke("runWithoutCallback", {"no.such"});
        // The answer comes, and the process is alive for what follows.
        QTest::qWait(300);
        Invoke("run", {"fixture.echo"});
        QVERIFY2(!WaitForTag("fixture.echo").contains("timedOut"), "no answer");
    }

    void AnExceptionInACallbackIsLoggedAndTheNextCallbackRuns() {
        QVERIFY(Setup());
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression("roost: the callback of commands.run threw: "
                               ".*boom"));
        Invoke("runThrowing", {"fixture.echo"});
        QTest::qWait(300);
        Invoke("run", {"fixture.echo"});
        QVERIFY2(!WaitForTag("fixture.echo").contains("timedOut"), "no answer");
    }

    void TheStoreGetsSetsAndTellsWhatChanged() {
        QVERIFY(Setup());
        Invoke("storeGet", {"fixture"});
        const QJsonObject got = WaitForTag("get");
        QVERIFY2(!got.contains("timedOut"), "no answer");
        QVERIFY(got.value("err").isNull());
        QCOMPARE(got.value("res"),
                 QJsonValue(QJsonObject{{"document", QJsonObject{}}}));
        Invoke("storeWatch");

        Invoke("storeSet", {"fixture", Doc(1)});
        const QJsonObject set = WaitForTag("set");
        QVERIFY2(!set.contains("timedOut"), "no answer");
        QVERIFY(set.value("err").isNull());
        const QJsonObject changed = WaitForTag("changed");
        QVERIFY2(!changed.contains("timedOut"), "no change");
        // rec(tag, err, res) was given the plugin and the document.
        QCOMPARE(changed.value("err").toString(), QString("fixture"));
        QCOMPARE(changed.value("res"), QJsonValue(Doc(1)));

        // A write of another window is heard the same way.
        const auto second = ConnectApi();
        QVERIFY(second);
        QVERIFY(Wait(second->Store().Set("fixture", Doc(2))));
        QTRY_COMPARE_WITH_TIMEOUT(CountTag("changed"), 2, TIMEOUT_MS);
        QCOMPARE(Got().last().value("res"), QJsonValue(Doc(2)));
        // And a read after it is the new document.
        Invoke("storeGet", {"fixture"});
        QTRY_COMPARE_WITH_TIMEOUT(CountTag("get"), 2, TIMEOUT_MS);
        QCOMPARE(Got().last().value("res"),
                 QJsonValue(QJsonObject{{"document", Doc(2)}}));
    }

    void AValueIsStoredAsJsonStringifyHasIt() {
        QVERIFY(Setup());
        Invoke("setConverted", {"fixture"});
        QVERIFY2(!WaitForTag("set").contains("timedOut"), "no answer");
        Invoke("storeGet", {"fixture"});
        const QJsonObject got = WaitForTag("get");
        QVERIFY2(!got.contains("timedOut"), "no answer");
        // Members that are undefined or functions are gone, in an array they
        // are null, a Date is ISO text in UTC.
        const QJsonObject expected{
            {"keep", 1},
            {"nothing", QJsonValue::Null},
            {"when", "2020-01-02T03:04:05.000Z"},
            {"list", QJsonArray{QJsonValue::Null, QJsonValue::Null, 2}},
            {"nested", QJsonObject{{"keep", "x"}}},
        };
        QCOMPARE(got.value("res").toObject().value("document"),
                 QJsonValue(expected));
    }

    void ACallbackNeverRunsInsideTheCallThatAsksForIt() {
        QVERIFY(Setup());
        // The dispatcher answers at once: no such method.
        QVERIFY(!Invoke("runsInside", {"nothing", QJsonObject{}}).toBool());
        QVERIFY(
            !Invoke("runsInside", {"commands.run", QJsonObject{}}).toBool());
        QTRY_COMPARE_WITH_TIMEOUT(CountTag("later:nothing"), 1, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(CountTag("later:commands.run"), 1,
                                  TIMEOUT_MS);
        QCOMPARE(WaitForTag("later:nothing")
                     .value("err")
                     .toObject()
                     .value("code")
                     .toInt(),
                 METHOD_NOT_FOUND);
        // A document that the store has already is answered from its cache.
        Invoke("storeGet", {"fixture"});
        QVERIFY2(!WaitForTag("get").contains("timedOut"), "no answer");
        QVERIFY(!Invoke("getRunsInside", {"fixture"}).toBool());
        QTRY_COMPARE_WITH_TIMEOUT(CountTag("later:get"), 1, TIMEOUT_MS);
        QVERIFY(WaitForTag("later:get").value("err").isNull());
        QCOMPARE(Probe_.OutsideGuiThread, 0);
    }

    void ACallBeforeTheApiIsSetFailsLaterNotInside() {
        QVERIFY(SetupPage(false));
        QVERIFY(!Invoke("runsInside", {"nothing", QJsonObject{}}).toBool());
        const QJsonObject record = WaitForTag("later:nothing");
        QVERIFY2(!record.contains("timedOut"), "no answer");
        QCOMPARE(record.value("err").toObject().value("code").toInt(),
                 static_cast<int>(rpc::EErrorCode::InternalError));
    }

    void ARefusedStoreWriteIsAnErrorInTheCallback() {
        QVERIFY(Setup());
        Invoke("storeSet", {"fixture", 5});
        const QJsonObject set = WaitForTag("set");
        QVERIFY2(!set.contains("timedOut"), "no answer");
        QCOMPARE(set.value("err").toObject().value("code").toInt(),
                 INVALID_PARAMS);
    }

    void TheBusSubscribesPublishesAndUnsubscribes() {
        QVERIFY(Setup());
        Invoke("makeOwner", {"panel"});
        const double id =
            Invoke("subscribe", {"x.*", "panel", "prefix"}).toNumber();
        QVERIFY(id > 0);
        QVERIFY(Invoke("subscribe", {"*", "panel", "all"}).toNumber() > id);
        Invoke("publish", {"x.a", QJsonObject{{"n", 1}}});
        const QJsonObject heard = WaitForTag("prefix");
        QVERIFY2(!heard.contains("timedOut"), "no event");
        // callback(name, payload).
        QCOMPARE(heard.value("err").toString(), QString("x.a"));
        QCOMPARE(heard.value("res"), QJsonValue(QJsonObject{{"n", 1}}));
        QVERIFY2(!WaitForTag("publish").contains("timedOut"), "no answer");
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("all"), 1, TIMEOUT_MS);
        QCOMPARE(Probe_.OutsideGuiThread, 0);

        // After unsubscribe the first listener is silent; the second hears.
        // y.marker is for the second only, and the events of one connection
        // come in order: when it is heard, x.b has had its turn.
        Invoke("unsubscribe", {id});
        Invoke("publish", {"x.b", 2});
        Invoke("publish", {"y.marker", 3});
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("all"), 3, TIMEOUT_MS);
        QCOMPARE(Probe_.Count("prefix"), 1);
    }

    void AnEventsOwnerDestroyedStopsTheDelivery() {
        QVERIFY(Setup());
        Invoke("makeOwner", {"gone"});
        Invoke("makeOwner", {"kept"});
        QVERIFY(Invoke("subscribe", {"x.y", "gone", "gone"}).toNumber() > 0);
        QVERIFY(Invoke("subscribe", {"x.y", "kept", "kept"}).toNumber() > 0);
        Invoke("publish", {"x.y", 1});
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("kept"), 1, TIMEOUT_MS);
        QCOMPARE(Probe_.Count("gone"), 1);

        Invoke("killOwner", {"gone"});
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        Invoke("publish", {"x.y", 2});
        // The listener of "kept" came after that of "gone": it heard the
        // second event, so the other had its turn.
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("kept"), 2, TIMEOUT_MS);
        QCOMPARE(Probe_.Count("gone"), 1);
    }

    void BadArgumentsOfSubscribeThrowATypeError() {
        QVERIFY(Setup());
        for (const char* what :
             {"null owner", "bad pattern", "not a function"}) {
            QCOMPARE(Invoke("thrownBy", {what}).toString(),
                     QString("TypeError"));
        }
    }

    void EveryMethodOfTheDispatcherIsReachableByName() {
        QVERIFY(Setup());
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
            Invoke("call", {method, params.value(method)});
            const QJsonObject record = WaitForTag(method);
            QVERIFY2(!record.contains("timedOut"), qPrintable(method));
            QVERIFY2(record.value("err").isNull(), qPrintable(method));
            QVERIFY2(record.contains("res"), qPrintable(method));
        }
        QCOMPARE(WaitForTag("commands.run").value("res"),
                 QJsonValue(QJsonArray{1, "two"}));
        // What the window keeps for itself is not there.
        for (const char* method :
             {"hello", "session.open", "session.close", "nothing"}) {
            Invoke("call", {method, QJsonObject{}});
            const QJsonObject record = WaitForTag(method);
            QVERIFY2(!record.contains("timedOut"), method);
            QCOMPARE(record.value("err").toObject().value("code").toInt(),
                     METHOD_NOT_FOUND);
        }
    }

    void ConnectedFollowsTheConnectionToRoostd() {
        QVERIFY(Setup());
        QVERIFY(Invoke("isConnected").toBool());
        QSignalSpy changes(Singleton_, &RoostSingleton::connectedChanged);
        QVERIFY(Daemon_->Stop());
        QTRY_COMPARE_WITH_TIMEOUT(changes.size(), 1, TIMEOUT_MS);
        QVERIFY(!Invoke("isConnected").toBool());
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QTRY_COMPARE_WITH_TIMEOUT(changes.size(), 2, TIMEOUT_MS);
        QVERIFY(Invoke("isConnected").toBool());
    }

    void APendingCallFailsWithConnectionLostWhenRoostdStops() {
        QVERIFY(Setup());
        Invoke("runHanging");
        // The request behind it is answered, so roostd has had the first.
        QVERIFY(Wait(Api_->Bus().Publish("probe", 1)));
        QCOMPARE(CountTag("hang"), 0);
        QVERIFY(Daemon_->Stop());
        const QJsonObject record = WaitForTag("hang");
        QVERIFY2(!record.contains("timedOut"), "no answer");
        QCOMPARE(record.value("err").toObject().value("code").toInt(),
                 CONNECTION_LOST);
    }

    void ACallWhoseEngineIsGoneIsNotAnswered() {
        QVERIFY(Setup());
        Invoke("runHanging");
        QVERIFY(Wait(Api_->Bus().Publish("probe", 1)));
        QSignalSpy disconnected(Api_.get(), &api::Api::Disconnected);
        Page_.reset();
        Engine_.reset();
        Singleton_ = nullptr;
        // The answer, ConnectionLost, arrives with no one to take it.
        QVERIFY(Daemon_->Stop());
        QVERIFY(disconnected.wait(TIMEOUT_MS));
        QTest::qWait(200);
        QCOMPARE(Probe_.Count("hang"), 0);
    }
};

QTEST_MAIN(RoostCallsTest)
#include "roost_calls_test.moc"
