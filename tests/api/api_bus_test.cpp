// The bus service of the window against the real roostd: patterns, the one
// roostd subscription per pattern, unsubscribing, the end of a context, and
// the races between an answer and the events that follow it.

#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include "api/api.hpp"
#include "api/thread_check.hpp"
#include "api_test_support.hpp"
#include "core/protocol.hpp"
#include "helpers/roostd_process.hpp"

using namespace roost;
using namespace roost::api_test;

namespace {

struct Event {
    QString Name;
    QJsonValue Payload;
    bool operator==(const Event&) const = default;
};

using Events = std::vector<Event>;

// Counts the warnings of a failed bus.subscribe, and passes the rest on.
std::atomic<int> SUBSCRIBE_WARNINGS{0};
QtMessageHandler PREVIOUS_HANDLER = nullptr;

void CountSubscribeWarnings(QtMsgType type, const QMessageLogContext& context,
                            const QString& message) {
    if (type == QtWarningMsg && message.contains("bus.subscribe") &&
        message.contains("its listeners are dropped")) {
        ++SUBSCRIBE_WARNINGS;
        return;
    }
    PREVIOUS_HANDLER(type, context, message);
}

}  // namespace

class ApiBusTest : public QObject {
    Q_OBJECT

private:
    // Records into `into` what a listener hears.
    api::BusService::Listener Record(Events& into) {
        return [&into](const QString& name, const QJsonValue& payload) {
            into.push_back({name, payload});
        };
    }

    api::BusService::LocalId Subscribe(const QString& pattern,
                                       const QObject* context,
                                       api::BusService::Listener listener) {
        const auto id =
            Api_->Bus().Subscribe(pattern, context, std::move(listener));
        return id ? *id : 0;
    }

    void Publish(const QString& name, const QJsonValue& payload = 1) {
        const QFuture<rpc::Result> published =
            Api_->Bus().Publish(name, payload);
        QVERIFY(WaitFor(published));
        QVERIFY2(published.result(), qPrintable(Why(published.result())));
    }

    // Everything published before has been delivered once a listener hears an
    // event published now: roostd and the IO thread keep the order.
    void Flush() {
        Events seen;
        const auto id = Subscribe("flush", this, Record(seen));
        Publish("flush");
        QTRY_VERIFY_WITH_TIMEOUT(!seen.empty(), TIMEOUT_MS);
        Api_->Bus().Unsubscribe(id);
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

private slots:
    void init() {
        Daemon_ = std::make_unique<roost::testing::RoostdProcess>();
        QVERIFY(Daemon_->IsValid());
        QVERIFY(Daemon_->AddFixturePlugin("fixture", FIXTURE_BACKEND_PATH));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        Api_ = std::make_unique<api::Api>();
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        Api_->Connect(Daemon_->SocketPath());
        QVERIFY(connected.wait(TIMEOUT_MS));
    }

    void cleanup() {
        Api_.reset();
        Daemon_.reset();
    }

    void AnExactPatternHearsThatName() {
        Events heard;
        QVERIFY(Subscribe("x.a", this, Record(heard)));
        Publish("x.a", 1);
        Publish("x.b", 2);
        Publish("x", 3);
        Publish("x.a.b", 4);
        Publish("x.a", QJsonObject{{"k", "v"}});
        Flush();
        QCOMPARE(heard, (Events{{"x.a", 1}, {"x.a", QJsonObject{{"k", "v"}}}}));
    }

    void APrefixPatternHearsEverythingUnderIt() {
        Events heard;
        QVERIFY(Subscribe("x.*", this, Record(heard)));
        Publish("x.a", 1);
        Publish("x.b.c", 2);
        Publish("x", 3);
        Publish("y.a", 4);
        Flush();
        QCOMPARE(heard, (Events{{"x.a", 1}, {"x.b.c", 2}}));
    }

    void StarHearsEverything() {
        Events heard;
        QVERIFY(Subscribe("*", this, Record(heard)));
        Publish("x.a", 1);
        Publish("y", QJsonValue());
        Flush();
        // The flush event is heard too.
        QCOMPARE(heard,
                 (Events{{"x.a", 1}, {"y", QJsonValue()}, {"flush", 1}}));
    }

    void ListenersOfOnePatternShareOneRoostdSubscription() {
        Events first;
        Events second;
        Events other;
        const auto a = Subscribe("x.a", this, Record(first));
        const auto b = Subscribe("x.a", this, Record(second));
        QVERIFY(Subscribe("x.b", this, Record(other)));
        QVERIFY(a != b);
        Publish("x.a", 1);
        Flush();
        QCOMPARE(first, (Events{{"x.a", 1}}));
        QCOMPARE(second, (Events{{"x.a", 1}}));
        QVERIFY(other.empty());

        // One listener leaves: the pattern is still there.
        Api_->Bus().Unsubscribe(a);
        Publish("x.a", 2);
        Flush();
        QCOMPARE(first.size(), size_t{1});
        QCOMPARE(second.size(), size_t{2});
        // Two patterns, not three subscriptions.
        QCOMPARE(SubscriptionsAtRoostd(), 2);
    }

    void TheLastListenerTakesTheRoostdSubscriptionWithIt() {
        Events first;
        Events second;
        const auto a = Subscribe("x.a", this, Record(first));
        const auto b = Subscribe("x.a", this, Record(second));
        Flush();
        Api_->Bus().Unsubscribe(a);
        Api_->Bus().Unsubscribe(b);
        // Unsubscribing twice is nothing.
        Api_->Bus().Unsubscribe(b);
        Flush();
        QCOMPARE(SubscriptionsAtRoostd(), 0);
    }

    void UnsubscribeStopsDelivery() {
        Events heard;
        const auto id = Subscribe("x.*", this, Record(heard));
        Publish("x.a", 1);
        Flush();
        QCOMPARE(heard.size(), size_t{1});
        Api_->Bus().Unsubscribe(id);
        Publish("x.a", 2);
        Flush();
        QCOMPARE(heard.size(), size_t{1});
    }

    void AListenerMayUnsubscribeItselfAndOthers() {
        Events heard;
        api::BusService::LocalId second = 0;
        api::BusService::LocalId first = 0;
        first =
            Subscribe("x.a", this, [&](const QString& name, const QJsonValue&) {
                heard.push_back({"first:" + name, {}});
                Api_->Bus().Unsubscribe(first);
                Api_->Bus().Unsubscribe(second);
            });
        second = Subscribe("x.a", this, Record(heard));
        Publish("x.a", 1);
        Publish("x.a", 2);
        Flush();
        QCOMPARE(heard, (Events{{"first:x.a", {}}}));
    }

    void NothingIsHeardAfterTheContextIsDestroyed() {
        auto context = std::make_unique<QObject>();
        Events heard;
        Events alive;
        QVERIFY(Subscribe("x.a", context.get(), Record(heard)));
        QVERIFY(Subscribe("x.b", context.get(), Record(heard)));
        QVERIFY(Subscribe("x.a", this, Record(alive)));
        Publish("x.a", 1);
        Flush();
        QCOMPARE(heard.size(), size_t{1});
        context.reset();
        Publish("x.a", 2);
        Publish("x.b", 3);
        Flush();
        QCOMPARE(heard.size(), size_t{1});
        QCOMPARE(alive.size(), size_t{2});
        // Only the pattern that still has a listener is left at roostd.
        QCOMPARE(SubscriptionsAtRoostd(), 1);
    }

    void AContextDestroyedBeforeTheAnswerIsAllowed() {
        auto context = std::make_unique<QObject>();
        Events heard;
        QVERIFY(Subscribe("x.a", context.get(), Record(heard)));
        context.reset();
        Publish("x.a", 1);
        Flush();
        QVERIFY(heard.empty());
        QCOMPARE(SubscriptionsAtRoostd(), 0);
    }

    void AnInvalidPatternIsRefused() {
        Events heard;
        for (const char* pattern :
             {"", "x..y", "x.*.y", "*.x", ".x", "x.", "has space", "x*"}) {
            const auto id = Api_->Bus().Subscribe(pattern, this, Record(heard));
            QVERIFY2(!id, pattern);
            QCOMPARE(id.error().Code,
                     static_cast<int>(rpc::EErrorCode::InvalidParams));
        }
        // A context in another thread is refused too.
        QThread thread;
        QObject foreign;
        foreign.moveToThread(&thread);
        QVERIFY(!Api_->Bus().Subscribe("x", &foreign, Record(heard)));
        QCOMPARE(SubscriptionsAtRoostd(), 0);
    }

    void UnsubscribeWhileTheAnswerIsOnItsWay() {
        Events heard;
        const auto id = Subscribe("x.a", this, Record(heard));
        Api_->Bus().Unsubscribe(id);
        Flush();
        Publish("x.a", 1);
        Flush();
        QVERIFY(heard.empty());
        QCOMPARE(SubscriptionsAtRoostd(), 0);
    }

    void ASubscriberAfterAnUnsubscribeInFlightTakesTheSubscriptionOver() {
        Events gone;
        Events heard;
        Api_->Bus().Unsubscribe(Subscribe("x.a", this, Record(gone)));
        // The answer is still pending; the pattern has no listener now.
        QVERIFY(Subscribe("x.a", this, Record(heard)));
        Flush();
        Publish("x.a", 1);
        Flush();
        QVERIFY(gone.empty());
        QCOMPARE(heard, (Events{{"x.a", 1}}));
        QCOMPARE(SubscriptionsAtRoostd(), 1);
    }

    void AnEventThatOvertakesTheAnswerIsNotLost() {
        // The subscription ids of roostd count up: learn the next one.
        const auto probe = Api_->Io().Request(
            "bus.subscribe", QJsonObject{{"pattern", "probe"}});
        QVERIFY(WaitFor(probe));
        QVERIFY(probe.result());
        const qint64 next =
            probe.result()->toObject().value("subscription").toInteger() + 1;

        Events heard;
        QVERIFY(Subscribe("x.a", this, Record(heard)));
        // Before the event loop runs, so before the answer: the event of the
        // subscription arrives, and an event of a subscription nobody has.
        emit Api_->Io().Notified(
            "bus.event",
            QJsonObject{
                {"subscription", next}, {"name", "x.a"}, {"payload", "early"}});
        emit Api_->Io().Notified("bus.event",
                                 QJsonObject{{"subscription", next + 100},
                                             {"name", "x.a"},
                                             {"payload", "stranger"}});
        QVERIFY(heard.empty());
        Flush();
        QCOMPARE(heard, (Events{{"x.a", "early"}}));
    }

    void ASubscribeFromAnotherThreadIsRefused() {
#ifdef NDEBUG
        QSKIP("the check is compiled out of release builds");
#endif
        static std::atomic<int> calls{0};
        calls = 0;
        const auto previous =
            api::SetCheckHandler([](const char*) { ++calls; });
        Events heard;
        std::expected<api::BusService::LocalId, rpc::Error> id;
        std::unique_ptr<QThread> thread(QThread::create([&] {
            id = Api_->Bus().Subscribe("x.a", nullptr, Record(heard));
        }));
        thread->start();
        QVERIFY(thread->wait(TIMEOUT_MS));
        api::SetCheckHandler(previous);

        QCOMPARE(calls.load(), 1);
        QVERIFY(!id);
        // Nothing reached roostd, and nothing is heard.
        Publish("x.a", 1);
        Flush();
        QVERIFY(heard.empty());
        QCOMPARE(SubscriptionsAtRoostd(), 0);
    }

    void AnEventOfADeadSubscriptionDoesNotReachANewListener() {
        // The next subscription of roostd is the probe's plus one.
        const auto probe = Api_->Io().Request(
            "bus.subscribe", QJsonObject{{"pattern", "probe"}});
        QVERIFY(WaitFor(probe));
        QVERIFY(probe.result());
        const qint64 old =
            probe.result()->toObject().value("subscription").toInteger() + 1;

        Events gone;
        const auto first = Subscribe("x.a", this, Record(gone));
        // Answered, so that the subscription is routed.
        Flush();
        Api_->Bus().Unsubscribe(first);
        Events heard;
        QVERIFY(Subscribe("x.a", this, Record(heard)));
        Flush();

        // A late event of the first subscription, which no longer exists.
        emit Api_->Io().Notified("bus.event", QJsonObject{{"subscription", old},
                                                          {"name", "x.a"},
                                                          {"payload", "late"}});
        QVERIFY(heard.empty());
        Publish("x.a", 1);
        Flush();
        QVERIFY(gone.empty());
        QCOMPARE(heard, (Events{{"x.a", 1}}));
    }

    void ASecondConnectChangesNothing() {
        Events heard;
        QVERIFY(Subscribe("x.a", this, Record(heard)));
        Flush();
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        QSignalSpy disconnected(Api_.get(), &api::Api::Disconnected);
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression("Api::Connect is called once"));
        Api_->Connect(Daemon_->SocketPath());
        QVERIFY(!connected.wait(500));
        QVERIFY(disconnected.isEmpty());
        QVERIFY(Api_->IsConnected());
        Publish("x.a", 1);
        Flush();
        QCOMPARE(heard, (Events{{"x.a", 1}}));
    }

    void AfterTheDaemonIsGoneASubscribeIsRefusedAndItsListenersDropped() {
        Events before;
        Events known;
        Events fresh;
        const auto old = Subscribe("x.a", this, Record(before));
        Flush();
        QSignalSpy disconnected(Api_.get(), &api::Api::Disconnected);
        Daemon_->Stop();
        QVERIFY(disconnected.wait(TIMEOUT_MS));

        SUBSCRIBE_WARNINGS = 0;
        PREVIOUS_HANDLER = qInstallMessageHandler(&CountSubscribeWarnings);
        // A pattern that was active before the loss, and a new one.
        const auto again = Subscribe("x.a", this, Record(known));
        const auto other = Subscribe("x.b", this, Record(fresh));
        QVERIFY(again && other);
        QTRY_COMPARE_WITH_TIMEOUT(SUBSCRIBE_WARNINGS.load(), 2, TIMEOUT_MS);
        // The failure left no stale state: the same pattern fails again.
        const auto third = Subscribe("x.b", this, Record(fresh));
        QTRY_COMPARE_WITH_TIMEOUT(SUBSCRIBE_WARNINGS.load(), 3, TIMEOUT_MS);
        Api_->Bus().Unsubscribe(third);
        qInstallMessageHandler(PREVIOUS_HANDLER);

        // Dropped: nothing to unsubscribe, nothing was or will be called.
        Api_->Bus().Unsubscribe(again);
        Api_->Bus().Unsubscribe(other);
        // The listener from before the loss is still there, and silent.
        Api_->Bus().Unsubscribe(old);
        QVERIFY(before.empty());
        QVERIFY(known.empty());
        QVERIFY(fresh.empty());
    }

    void StoreChangedArrivesThroughTheBus() {
        Events heard;
        QVERIFY(Subscribe("store.changed", this, Record(heard)));
        const QJsonObject document{{"pins", 3}};
        const QFuture<rpc::Result> stored = Api_->Dispatch().Call(
            "store.set",
            QJsonObject{{"plugin", "fixture"}, {"document", document}}, this);
        QVERIFY(WaitFor(stored));
        QVERIFY2(stored.result(), qPrintable(Why(stored.result())));
        QTRY_COMPARE_WITH_TIMEOUT(heard.size(), size_t{1}, TIMEOUT_MS);
        QCOMPARE(heard[0].Name, QString("store.changed"));
        QCOMPARE(heard[0].Payload,
                 QJsonValue(QJsonObject{{"plugin", "fixture"},
                                        {"document", document}}));
    }

    void ListenersRunInTheGuiThread() {
        QThread* heardIn = nullptr;
        QVERIFY(Subscribe("x.a", this, [&](const QString&, const QJsonValue&) {
            heardIn = QThread::currentThread();
        }));
        Publish("x.a");
        QTRY_VERIFY_WITH_TIMEOUT(heardIn != nullptr, TIMEOUT_MS);
        QCOMPARE(heardIn, QThread::currentThread());
    }
};

QTEST_GUILESS_MAIN(ApiBusTest)
#include "api_bus_test.moc"
