// Every public method of the services and of the dispatcher checks that it is
// called from the GUI thread (debug builds only).

#include <QJsonObject>
#include <QTest>
#include <QThread>

#include <memory>
#include <mutex>
#include <vector>

#include "api/api.hpp"
#include "api/thread_check.hpp"

using namespace roost;

namespace {

std::mutex MESSAGES_MUTEX;
std::vector<QString> MESSAGES;

void Record(const char* message) {
    const std::scoped_lock lock(MESSAGES_MUTEX);
    MESSAGES.emplace_back(message);
}

QStringList Recorded() {
    const std::scoped_lock lock(MESSAGES_MUTEX);
    return QStringList(MESSAGES.begin(), MESSAGES.end());
}

// Every public method, once. What they return is not looked at.
void CallEverything(api::Api& api) {
    const rpc::Handler handler = [](const rpc::Call&) {
        return rpc::Ready(QJsonValue());
    };
    const auto listener = [](const QString&, const QJsonValue&) {};
    api.IsConnected();
    api.Bus().Subscribe("x", nullptr, listener);
    api.Bus().Unsubscribe(1);
    api.Bus().Publish("x", {});
    api.Commands().Run("c");
    api.Plugins().Fetch();
    api.Plugins().IsLoaded();
    api.Plugins().Plugins();
    api.Plugins().List();
    (void)api.Dispatch().Register("t.x", handler);
    api.Dispatch().Methods();
    api.Dispatch().Call("sessions.list", {}, &api);
    api.Dispatch().Subscribe("x", nullptr, listener);
    api.Dispatch().Unsubscribe(1);
}

constexpr int CALLS = 14;

}  // namespace

class ApiThreadCheckTest : public QObject {
    Q_OBJECT

private slots:
    void init() {
        const std::scoped_lock lock(MESSAGES_MUTEX);
        MESSAGES.clear();
    }

    void ACallFromAnotherThreadCallsTheHandler() {
#ifdef NDEBUG
        QSKIP("the check is compiled out of release builds");
#endif
        api::Api api;
        const auto previous = api::SetCheckHandler(&Record);
        std::unique_ptr<QThread> thread(
            QThread::create([&api] { CallEverything(api); }));
        thread->start();
        QVERIFY(thread->wait(10000));
        api::SetCheckHandler(previous);

        const QStringList messages = Recorded();
        QCOMPARE(messages.size(), CALLS);
        for (const char* method :
             {"Api::IsConnected", "BusService::Subscribe",
              "BusService::Unsubscribe", "BusService::Publish",
              "CommandService::Run", "PluginService::Fetch",
              "PluginService::IsLoaded", "PluginService::Plugins",
              "PluginService::List", "Dispatcher::Register",
              "Dispatcher::Methods", "Dispatcher::Call",
              "Dispatcher::Subscribe", "Dispatcher::Unsubscribe"}) {
            QVERIFY2(messages.filter(method).size() >= 1, method);
        }
        // The methods that failed their check did nothing: the dispatcher
        // still lacks the method. That a refused Subscribe makes no
        // bus.subscribe is checked in api_bus_test.
        QVERIFY(!api.Dispatch().Methods().contains("t.x"));
        QVERIFY(!api.Plugins().IsLoaded());
    }

    void ASubscribeFromAnotherThreadIsRefused() {
#ifdef NDEBUG
        QSKIP("the check is compiled out of release builds");
#endif
        api::Api api;
        const auto previous = api::SetCheckHandler(&Record);
        bool heard = false;
        std::expected<api::BusService::LocalId, rpc::Error> id;
        std::unique_ptr<QThread> thread(QThread::create([&] {
            id = api.Bus().Subscribe(
                "x", nullptr,
                [&heard](const QString&, const QJsonValue&) { heard = true; });
        }));
        thread->start();
        QVERIFY(thread->wait(10000));
        api::SetCheckHandler(previous);

        QCOMPARE(Recorded().size(), 1);
        QVERIFY(!id);
        QVERIFY(!heard);
    }

    void ACallFromTheGuiThreadDoesNot() {
        api::Api api;
        const auto previous = api::SetCheckHandler(&Record);
        CallEverything(api);
        api::SetCheckHandler(previous);
        QCOMPARE(Recorded(), QStringList());
    }

    void TheDefaultHandlerIsRestoredByNull() {
        const auto previous = api::SetCheckHandler(&Record);
        QVERIFY(api::SetCheckHandler(nullptr) == &Record);
        // The default again: replacing it returns what was there.
        const auto fatal = api::SetCheckHandler(&Record);
        QVERIFY(fatal != nullptr);
        QVERIFY(fatal != &Record);
        api::SetCheckHandler(previous);
    }
};

QTEST_GUILESS_MAIN(ApiThreadCheckTest)
#include "api_thread_check_test.moc"
