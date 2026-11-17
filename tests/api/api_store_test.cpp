// The document cache of the window against the real roostd, with plugins
// whose backend is the fixture backend: Get fills it, store.changed keeps it
// fresh, whoever wrote, Set leaves it to the event, and roostd's errors pass
// unchanged.

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <memory>

#include "api/api.hpp"
#include "api/thread_check.hpp"
#include "api_test_support.hpp"
#include "core/protocol.hpp"
#include "helpers/roostd_process.hpp"

using namespace roost;
using namespace roost::api_test;

namespace {

constexpr int STORE_FAILED =
    static_cast<int>(protocol::EErrorCode::StoreFailed);
constexpr int CONNECTION_LOST =
    static_cast<int>(protocol::EErrorCode::ConnectionLost);
constexpr int INVALID_PARAMS = static_cast<int>(rpc::EErrorCode::InvalidParams);

QJsonObject Doc(int pins) { return {{"pins", pins}}; }

QJsonObject Wrapped(const QJsonObject& document) {
    return {{"document", document}};
}

}  // namespace

class ApiStoreTest : public QObject {
    Q_OBJECT

private:
    bool StartDaemon() {
        return Daemon_->IsValid() &&
               Daemon_->AddFixturePlugin("fixture", FIXTURE_BACKEND_PATH) &&
               Daemon_->AddFixturePlugin("second", FIXTURE_BACKEND_PATH) &&
               Daemon_->Start(ROOSTD_PATH);
    }

    std::unique_ptr<api::Api> ConnectApi() {
        auto api = std::make_unique<api::Api>();
        QSignalSpy connected(api.get(), &api::Api::Connected);
        api->Connect(Daemon_->SocketPath());
        return connected.wait(TIMEOUT_MS) ? std::move(api) : nullptr;
    }

    static rpc::Result Wait(const QFuture<rpc::Result>& future) {
        if (!WaitFor(future)) {
            return std::unexpected(rpc::Error{.Message = "timed out"});
        }
        return future.result();
    }

    static rpc::Result Get(api::Api& api, const QString& plugin) {
        return Wait(api.Store().Get(plugin));
    }

    static rpc::Result Set(api::Api& api, const QString& plugin,
                           const QJsonValue& document) {
        return Wait(api.Store().Set(plugin, document));
    }

    // What roostd holds, past the cache.
    static rpc::Result AtRoostd(api::Api& api, const QString& plugin) {
        return Wait(
            api.Io().Request("store.get", QJsonObject{{"plugin", plugin}}));
    }

    // Writes the file of the store behind roostd's back, so that what roostd
    // answers differs from what the cache holds.
    bool WriteBehindRoostd(const QString& plugin, const QJsonObject& document) {
        QFile file(Daemon_->ConfigPath() + "/" + plugin + ".json");
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               file.write(QJsonDocument(document).toJson()) > 0;
    }

    std::unique_ptr<roost::testing::RoostdProcess> Daemon_;

private slots:
    void init() { Daemon_ = std::make_unique<roost::testing::RoostdProcess>(); }

    void cleanup() { Daemon_.reset(); }

    void AnAbsentDocumentIsEmptyAndTheCacheWarm() {
        QVERIFY(StartDaemon());
        const auto api = ConnectApi();
        QVERIFY(api);
        QVERIFY(!api->Store().IsWarm("fixture"));
        const rpc::Result answer = Get(*api, "fixture");
        QVERIFY2(answer, qPrintable(Why(answer)));
        QCOMPARE(*answer, QJsonValue(Wrapped({})));
        QVERIFY(api->Store().IsWarm("fixture"));
        // Only the plugin that was read.
        QVERIFY(!api->Store().IsWarm("second"));
    }

    void ASetIsHeardAsAnEventAndTheNextGetComesFromTheCache() {
        QVERIFY(StartDaemon());
        const auto api = ConnectApi();
        QVERIFY(api);
        QVERIFY(Get(*api, "fixture"));
        QSignalSpy changed(&api->Store(), &api::StoreService::Changed);
        const rpc::Result saved = Set(*api, "fixture", Doc(1));
        QVERIFY2(saved, qPrintable(Why(saved)));
        QCOMPARE(*saved, QJsonValue());
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, TIMEOUT_MS);
        QCOMPARE(changed[0][0].toString(), QString("fixture"));
        QCOMPARE(changed[0][1].toJsonObject(), Doc(1));

        // The file now says something else: an answer that still says 1 did
        // not come from roostd.
        QVERIFY(WriteBehindRoostd("fixture", Doc(99)));
        QCOMPARE(*AtRoostd(*api, "fixture"), QJsonValue(Wrapped(Doc(99))));
        const rpc::Result cached = Get(*api, "fixture");
        QVERIFY2(cached, qPrintable(Why(cached)));
        QCOMPARE(*cached, QJsonValue(Wrapped(Doc(1))));
    }

    void ASecondWindowSeesTheWriteOfTheFirst() {
        QVERIFY(StartDaemon());
        const auto first = ConnectApi();
        const auto second = ConnectApi();
        QVERIFY(first && second);
        QVERIFY(Get(*first, "fixture"));
        QVERIFY(Get(*second, "fixture"));
        QSignalSpy changed(&second->Store(), &api::StoreService::Changed);
        QVERIFY(Set(*first, "fixture", Doc(5)));
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, TIMEOUT_MS);
        // Past the file, so that the answer is the second one's cache.
        QVERIFY(WriteBehindRoostd("fixture", Doc(99)));
        QCOMPARE(*Get(*second, "fixture"), QJsonValue(Wrapped(Doc(5))));
        QCOMPARE(*Get(*first, "fixture"), QJsonValue(Wrapped(Doc(5))));
        // Whole documents replace: the last writer wins. The first window
        // hears it on its own connection, which may lag the writer's.
        QSignalSpy firstChanged(&first->Store(), &api::StoreService::Changed);
        QVERIFY(Set(*second, "fixture", QJsonObject{{"other", true}}));
        QTRY_COMPARE_WITH_TIMEOUT(firstChanged.size(), 1, TIMEOUT_MS);
        QCOMPARE(*Get(*first, "fixture"),
                 QJsonValue(Wrapped({{"other", true}})));
    }

    void AFirstGetAfterAnotherWindowsWriteSeesIt() {
        QVERIFY(StartDaemon());
        const auto first = ConnectApi();
        const auto second = ConnectApi();
        QVERIFY(first && second);
        QVERIFY(Set(*first, "fixture", Doc(7)));
        QCOMPARE(*Get(*second, "fixture"), QJsonValue(Wrapped(Doc(7))));
        QVERIFY(Set(*first, "fixture", Doc(8)));
        QTRY_VERIFY_WITH_TIMEOUT(
            Get(*second, "fixture").value_or(QJsonValue()) ==
                QJsonValue(Wrapped(Doc(8))),
            TIMEOUT_MS);
    }

    void AnEventOfAPluginNobodyReadIsNotKept() {
        QVERIFY(StartDaemon());
        const auto first = ConnectApi();
        const auto second = ConnectApi();
        QVERIFY(first && second);
        // The second window listens to store.changed, for "fixture" only.
        QVERIFY(Get(*second, "fixture"));
        QSignalSpy changed(&second->Store(), &api::StoreService::Changed);
        QVERIFY(Set(*first, "second", Doc(3)));
        QVERIFY(Set(*first, "fixture", Doc(4)));
        // The event of "fixture" came after that of "second".
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, TIMEOUT_MS);
        QCOMPARE(changed[0][0].toString(), QString("fixture"));
        QVERIFY(!second->Store().IsWarm("second"));
        QCOMPARE(*Get(*second, "second"), QJsonValue(Wrapped(Doc(3))));
    }

    void ASetLeavesTheCacheToTheEvent() {
        QVERIFY(StartDaemon());
        const auto api = ConnectApi();
        QVERIFY(api);
        QVERIFY(Get(*api, "fixture"));
        QSignalSpy changed(&api->Store(), &api::StoreService::Changed);
        // Not waited for: the answer and the event are still on their way.
        const QFuture<rpc::Result> saving = api->Store().Set("fixture", Doc(1));
        const QFuture<rpc::Result> still = api->Store().Get("fixture");
        QVERIFY(still.isFinished());
        QCOMPARE(*still.result(), QJsonValue(Wrapped({})));
        QVERIFY(changed.isEmpty());
        QVERIFY(Wait(saving));
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, TIMEOUT_MS);
        QCOMPARE(*Get(*api, "fixture"), QJsonValue(Wrapped(Doc(1))));
    }

    void ARefusedSetChangesNothing() {
        QVERIFY(StartDaemon());
        const auto api = ConnectApi();
        QVERIFY(api);
        QVERIFY(Set(*api, "fixture", Doc(1)));
        QVERIFY(Get(*api, "fixture"));
        QSignalSpy changed(&api->Store(), &api::StoreService::Changed);
        for (const QJsonValue& notAnObject :
             {QJsonValue(5), QJsonValue("text"), QJsonValue(QJsonArray{1}),
              QJsonValue(), QJsonValue(QJsonValue::Undefined)}) {
            const rpc::Result refused = Set(*api, "fixture", notAnObject);
            QVERIFY(!refused);
            QCOMPARE(refused.error().Code, INVALID_PARAMS);
        }
        QVERIFY(!changed.wait(300));
        QCOMPARE(*Get(*api, "fixture"), QJsonValue(Wrapped(Doc(1))));
    }

    void TheErrorsOfRoostdPassUnchangedAndKeepNothing() {
        QVERIFY(StartDaemon());
        const auto api = ConnectApi();
        QVERIFY(api);
        // Not loaded, malformed, and the id that the keymap keeps for itself.
        for (const char* plugin :
             {"no-such-plugin", "Bad Id", "keymap", "../escape", ""}) {
            const rpc::Result got = Get(*api, plugin);
            const rpc::Result direct = AtRoostd(*api, plugin);
            QVERIFY2(!got, plugin);
            QCOMPARE(got.error().Code, INVALID_PARAMS);
            QCOMPARE(got.error().Message, direct.error().Message);
            QVERIFY(!api->Store().IsWarm(plugin));
            const rpc::Result set = Set(*api, plugin, QJsonObject{});
            QVERIFY2(!set, plugin);
            QCOMPARE(set.error().Code, INVALID_PARAMS);
            QVERIFY(!set.error().Message.isEmpty());
        }
        // A document that does not parse: roostd's own error, -32004.
        QVERIFY(QDir().mkpath(Daemon_->ConfigPath()));
        QFile broken(Daemon_->ConfigPath() + "/fixture.json");
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("{ not json");
        broken.close();
        const rpc::Result unreadable = Get(*api, "fixture");
        QVERIFY(!unreadable);
        QCOMPARE(unreadable.error().Code, STORE_FAILED);
        QVERIFY(!api->Store().IsWarm("fixture"));
    }

    void TheDispatcherChecksThePluginAndPassesTheRest() {
        QVERIFY(StartDaemon());
        const auto api = ConnectApi();
        QVERIFY(api);
        const auto call = [&](const QString& method,
                              const QJsonObject& params) {
            return Wait(api->Dispatch().Call(method, params, api.get()));
        };
        QCOMPARE(call("store.get", {{"plugin", 3}}).error().Code,
                 INVALID_PARAMS);
        QCOMPARE(call("store.get", {}).error().Code, INVALID_PARAMS);
        QCOMPARE(call("store.set", {{"document", Doc(1)}}).error().Code,
                 INVALID_PARAMS);
        QCOMPARE(call("store.set", {{"plugin", "keymap"}, {"document", Doc(1)}})
                     .error()
                     .Code,
                 INVALID_PARAMS);
        QVERIFY(call("store.get", {{"plugin", "fixture"}}));
        QVERIFY(api->Store().IsWarm("fixture"));
        QVERIFY(
            call("store.set", {{"plugin", "fixture"}, {"document", Doc(2)}}));
        QTRY_VERIFY_WITH_TIMEOUT(
            call("store.get", {{"plugin", "fixture"}}).value_or(QJsonValue()) ==
                QJsonValue(Wrapped(Doc(2))),
            TIMEOUT_MS);
    }

    void ALostConnectionDropsTheCache() {
        QVERIFY(StartDaemon());
        const auto api = ConnectApi();
        QVERIFY(api);
        QVERIFY(Get(*api, "fixture"));
        QVERIFY(api->Store().IsWarm("fixture"));
        QSignalSpy disconnected(api.get(), &api::Api::Disconnected);
        Daemon_->Stop();
        QVERIFY(disconnected.wait(TIMEOUT_MS));
        QVERIFY(!api->Store().IsWarm("fixture"));
        const rpc::Result got = Get(*api, "fixture");
        QVERIFY(!got);
        QCOMPARE(got.error().Code, CONNECTION_LOST);
        QVERIFY(!api->Store().IsWarm("fixture"));
    }

    void TheAnswerOfAGetBeforeTheConnectionIsNotKept() {
        QVERIFY(StartDaemon());
        api::Api api;
        QSignalSpy connected(&api, &api::Api::Connected);
        api.Connect(Daemon_->SocketPath());
        // Asked at once, ahead of hello: nothing would keep the document
        // fresh, so it is answered and not kept.
        const QFuture<rpc::Result> early = api.Store().Get("fixture");
        QVERIFY(connected.wait(TIMEOUT_MS));
        QVERIFY(Wait(early));
        QVERIFY(!api.Store().IsWarm("fixture"));
    }

    void ACallFromAnotherThreadIsRefused() {
#ifdef NDEBUG
        QSKIP("the check is compiled out of release builds");
#endif
        QVERIFY(StartDaemon());
        const auto api = ConnectApi();
        QVERIFY(api);
        const auto previous = api::SetCheckHandler(
            [](const char* /*message*/) { /* returns: the call is refused */ });
        QJsonArray seen;
        std::unique_ptr<QThread> thread(QThread::create([&] {
            auto& store = api->Store();
            store.Start();
            seen.append(store.IsWarm("fixture"));
            seen.append(store.Get("fixture").result().has_value());
            seen.append(store.Set("fixture", Doc(1)).result().has_value());
        }));
        thread->start();
        QVERIFY(thread->wait(10000));
        api::SetCheckHandler(previous);
        QCOMPARE(seen, (QJsonArray{false, false, false}));
        // Nothing reached roostd, and nothing was kept.
        QVERIFY(!api->Store().IsWarm("fixture"));
        QCOMPARE(*AtRoostd(*api, "fixture"), QJsonValue(Wrapped({})));
    }
};

QTEST_GUILESS_MAIN(ApiStoreTest)
#include "api_store_test.moc"
