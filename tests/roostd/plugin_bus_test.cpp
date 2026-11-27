#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <chrono>
#include <memory>

#include "core/frame.hpp"
#include "core/rpc_client.hpp"
#include "roostd/backend.hpp"
#include "roostd/plugins.hpp"
#include "roostd/server.hpp"

using namespace roost;
using namespace std::chrono_literals;

namespace {

constexpr int TIMEOUT_MS = 10000;

template <typename TValue>
bool WaitFor(const QFuture<TValue>& future) {
    if (future.isFinished()) {
        return true;
    }
    QEventLoop loop;
    QFutureWatcher<TValue> watcher;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop,
                     &QEventLoop::quit);
    QTimer::singleShot(TIMEOUT_MS, &loop, &QEventLoop::quit);
    watcher.setFuture(future);
    loop.exec();
    return future.isFinished();
}

// QVERIFY2 builds its message before it checks, so error() must not be called
// on a value.
QString MessageOf(const rpc::Result& result) {
    return result ? QString() : result.error().Message;
}

BackendOptions Fast() {
    return BackendOptions{
        .FirstRestartDelay = 10ms,
        .MaxRestartDelay = 10ms,
        .MaxRestarts = 5,
        .StableRun = 60s,
        .StopTimeout = 1s,
    };
}

QJsonObject Request(int id, const QString& method, const QJsonValue& params) {
    return {
        {"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}

QJsonObject Notification(const QString& method, const QJsonValue& params) {
    return {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
}

QJsonObject StateChanged(const QString& id, const QString& state) {
    return Notification(
        "session.stateChanged",
        QJsonObject{{"agent", "claude"},
                    {"session", QJsonObject{{"id", id}, {"state", state}}}});
}

// What the backend got as an event of a subscription.
QJsonObject Event(int subscription, const QString& name,
                  const QJsonValue& payload) {
    return Notification("bus.event", QJsonObject{{"subscription", subscription},
                                                 {"name", name},
                                                 {"payload", payload}});
}

}  // namespace

// The backend of a plugin on the bus (design.md §5): the real host and server
// around the fixture backend, which sends what a test tells it to and keeps
// what roostd sent it.
class PluginBusTest : public QObject {
    Q_OBJECT

private:
    QString PluginsPath() const { return Dir_->filePath("plugins"); }

    // A plugin whose backend is the fixture, serving the agent claude.
    void AddPlugin(const QString& id) {
        const QDir directory(PluginsPath() + u'/' + id);
        QVERIFY(directory.mkpath("."));
        QVERIFY(
            QFile::link(FIXTURE_BACKEND_PATH, directory.filePath("backend")));
        const QJsonObject manifest{
            {"id", id},
            {"version", "1"},
            {"api", PLUGIN_API},
            {"backend", QJsonObject{{"command", QJsonArray{"backend"}}}},
            {"contributes",
             QJsonObject{{"agents", QJsonArray{QJsonObject{
                                        {"id", "claude"},
                                        {"capabilities",
                                         QJsonArray{"nativeTuiAttach"}}}}}}},
        };
        QFile file(directory.filePath("plugin.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(manifest).toJson());
    }

    bool Start() {
        Server_ = std::make_unique<Server>();
        if (!Server_->Listen(Dir_->filePath("roostd.sock"))) {
            return false;
        }
        Host_ = std::make_unique<PluginHost>(*Server_, Fast());
        Host_->Load({PluginsPath()});
        Client_ = std::make_unique<RpcClient>();
        const QFuture<rpc::Result> hello =
            Client_->Connect(Dir_->filePath("roostd.sock"));
        return WaitFor(hello) && hello.result();
    }

    rpc::Result RunInBackend(const QString& command,
                             const QJsonValue& args = QJsonValue::Undefined,
                             const QString& plugin = "one") {
        QJsonObject params{{"command", command}};
        if (!args.isUndefined()) {
            params.insert("args", args);
        }
        const QFuture<rpc::Result> future =
            Host_->BackendOf(plugin)->Request("commands.run", params);
        if (!WaitFor(future)) {
            return std::unexpected(rpc::Error{.Message = "timed out"});
        }
        return future.result();
    }

    // Has the backend send these messages, in this order. Returns once roostd
    // has read all of them: the fixture answers after it wrote the last one.
    void BackendSends(const QJsonArray& messages,
                      const QString& plugin = "one") {
        const rpc::Result sent =
            RunInBackend("fixture.notify", messages, plugin);
        QVERIFY2(sent, qPrintable(MessageOf(sent)));
    }

    // What roostd sent the backend, since it started. The fixture reads its
    // input in order, so what roostd wrote before this request is in it.
    QJsonArray BackendReceived(const QString& plugin = "one") {
        const rpc::Result received =
            RunInBackend("fixture.received", QJsonValue::Undefined, plugin);
        if (!received) {
            QTest::qFail(qPrintable(MessageOf(received)), __FILE__, __LINE__);
            return {};
        }
        return received->toArray();
    }

    // The events of these names among what the backend got, in order: a case
    // counts what it is about, not what else the daemon publishes on its path.
    static QJsonArray EventsNamed(const QJsonArray& received,
                                  const QStringList& names) {
        QJsonArray events;
        for (const QJsonValue& message : received) {
            const QJsonObject object = message.toObject();
            if (object.value("method") == "bus.event" &&
                names.contains(object.value("params")
                                   .toObject()
                                   .value("name")
                                   .toString())) {
                events.append(object);
            }
        }
        return events;
    }

    // The answer the backend got to its request.
    static QJsonObject AnswerTo(const QJsonArray& received, int id) {
        for (const QJsonValue& message : received) {
            if (message.toObject().value("id").toInt(-1) == id &&
                !message.toObject().contains("method")) {
                return message.toObject();
            }
        }
        return {};
    }

    static QJsonObject ResultOf(int id, const QJsonValue& result) {
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
    }

    // Has the backend subscribe to the pattern as request `id`; the id of
    // the subscription.
    int BackendSubscribes(int id, const QString& pattern,
                          const QString& plugin = "one") {
        BackendSends(
            {Request(id, "bus.subscribe", QJsonObject{{"pattern", pattern}})},
            plugin);
        const QJsonObject answer = AnswerTo(BackendReceived(plugin), id);
        return answer.value("result")
            .toObject()
            .value("subscription")
            .toInt(-1);
    }

    std::unique_ptr<QTemporaryDir> Dir_;
    std::unique_ptr<Server> Server_;
    std::unique_ptr<PluginHost> Host_;
    std::unique_ptr<RpcClient> Client_;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
    }

    void cleanup() {
        Client_.reset();
        Host_.reset();
        Server_.reset();
    }

    void ABackendGetsTheEventsOfItsPattern() {
        AddPlugin("one");
        QVERIFY(Start());
        const std::size_t before = Server_->Bus().SubscriptionCount();
        const int subscription = BackendSubscribes(1, "session.*");
        QVERIFY(subscription > 0);
        QCOMPARE(Server_->Bus().SubscriptionCount(), before + 1);

        // The state change of a session of its own agent comes back to it;
        // events outside the pattern do not.
        BackendSends({StateChanged("s1", "blocked")});
        Server_->Bus().Publish("one.hello", QJsonValue());
        Server_->Bus().Publish("session", QJsonValue());
        Server_->Bus().Publish("sessions.x", QJsonValue());
        Server_->Bus().Publish("session.listed", QJsonObject{{"agent", "x"}});

        const QJsonArray all = BackendReceived();
        QCOMPARE(AnswerTo(all, 1),
                 ResultOf(1, QJsonObject{{"subscription", subscription}}));
        const QJsonArray received =
            EventsNamed(all, {"one.hello", "session", "sessions.x",
                              "session.stateChanged", "session.listed"});
        QCOMPARE(received.size(), 2);
        QCOMPARE(received[0].toObject(),
                 Event(subscription, "session.stateChanged",
                       QJsonObject{{"agent", "claude"},
                                   {"id", "s1"},
                                   {"title", ""},
                                   {"cwd", ""},
                                   {"state", "blocked"}}));
        QCOMPARE(received[1].toObject(), Event(subscription, "session.listed",
                                               QJsonObject{{"agent", "x"}}));
    }

    void AnEventReachesABackendOnceForEachSubscription() {
        AddPlugin("one");
        QVERIFY(Start());
        const int exact = BackendSubscribes(1, "fixture.ping");
        const int all = BackendSubscribes(2, "*");
        QVERIFY(exact > 0 && all > 0 && exact != all);
        Server_->Bus().Publish("fixture.ping", 7);

        const QJsonArray received =
            EventsNamed(BackendReceived(), {"fixture.ping"});
        QCOMPARE(received.size(), 2);
        QCOMPARE(received[0].toObject(), Event(exact, "fixture.ping", 7));
        QCOMPARE(received[1].toObject(), Event(all, "fixture.ping", 7));
    }

    void AnUnsubscribedBackendGetsNothingMore() {
        AddPlugin("one");
        QVERIFY(Start());
        const std::size_t before = Server_->Bus().SubscriptionCount();
        const int subscription = BackendSubscribes(1, "fixture.*");
        BackendSends({Request(2, "bus.unsubscribe",
                              QJsonObject{{"subscription", subscription}})});
        QCOMPARE(Server_->Bus().SubscriptionCount(), before);
        Server_->Bus().Publish("fixture.ping", QJsonValue());

        const QJsonArray received = BackendReceived();
        QCOMPARE(AnswerTo(received, 2), ResultOf(2, QJsonValue()));
        QVERIFY(EventsNamed(received, {"fixture.ping"}).isEmpty());
    }

    void ASubscriptionDiesWithTheProcessAndIsNotRestored() {
        AddPlugin("one");
        QVERIFY(Start());
        const std::size_t before = Server_->Bus().SubscriptionCount();
        QVERIFY(BackendSubscribes(1, "session.*") > 0);
        QVERIFY(BackendSubscribes(2, "fixture.*") > 0);
        QCOMPARE(Server_->Bus().SubscriptionCount(), before + 2);

        QVERIFY(!RunInBackend("fixture.crash"));
        // Gone with the process, before it is back.
        QCOMPARE(Server_->Bus().SubscriptionCount(), before);

        // The restarted process has asked for nothing and gets nothing.
        QTRY_VERIFY_WITH_TIMEOUT(RunInBackend("fixture.received").has_value(),
                                 TIMEOUT_MS);
        Server_->Bus().Publish("session.listed", QJsonObject{{"agent", "x"}});
        Server_->Bus().Publish("fixture.ping", QJsonValue());
        QVERIFY(BackendReceived().isEmpty());
        QCOMPARE(Server_->Bus().SubscriptionCount(), before);

        // It asks again, and gets each event once.
        const int subscription = BackendSubscribes(1, "session.*");
        QVERIFY(subscription > 0);
        QCOMPARE(Server_->Bus().SubscriptionCount(), before + 1);
        Server_->Bus().Publish("session.listed", QJsonObject{{"agent", "y"}});
        Server_->Bus().Publish("fixture.ping", QJsonValue());
        const QJsonArray received =
            EventsNamed(BackendReceived(), {"session.listed", "fixture.ping"});
        QCOMPARE(received.size(), 1);
        QCOMPARE(received[0].toObject(), Event(subscription, "session.listed",
                                               QJsonObject{{"agent", "y"}}));
    }

    void AnotherBackendIsLeftAlone() {
        AddPlugin("one");
        AddPlugin("two");
        QVERIFY(Start());
        const std::size_t before = Server_->Bus().SubscriptionCount();
        const int subscription = BackendSubscribes(1, "*", "two");
        QVERIFY(subscription > 0);
        QCOMPARE(Server_->Bus().SubscriptionCount(), before + 1);
        QVERIFY(!RunInBackend("fixture.crash"));
        QCOMPARE(Server_->Bus().SubscriptionCount(), before + 1);

        // It still gets its events.
        Server_->Bus().Publish("fixture.ping", 5);
        const QJsonArray received =
            EventsNamed(BackendReceived("two"), {"fixture.ping"});
        QCOMPARE(received.size(), 1);
        QCOMPARE(received[0].toObject(),
                 Event(subscription, "fixture.ping", 5));
    }

    void ABackendThatDoesNotReadItsEventsIsBroken() {
        AddPlugin("one");
        QVERIFY(Start());
        Backend* const backend = Host_->BackendOf("one");
        const std::size_t before = Server_->Bus().SubscriptionCount();
        QVERIFY(BackendSubscribes(1, "fixture.*") > 0);
        QCOMPARE(Server_->Bus().SubscriptionCount(), before + 1);

        QSignalSpy crashed(backend, &Backend::Crashed);
        const QFuture<rpc::Result> stalled = backend->Request(
            "commands.run", QJsonObject{{"command", "fixture.stall"}});
        const QString reason = "does not read its stdin";
        QTest::ignoreMessage(QtWarningMsg,
                             qPrintable("roostd: the backend of one " + reason +
                                        "; killing it"));
        QTest::ignoreMessage(QtWarningMsg,
                             qPrintable("roostd: the backend of one " + reason +
                                        "; restarting it in 10 ms"));
        // More than the backend can hold in its pipe and roostd may keep
        // for it.
        const QString payload(1024 * 1024, u'x');
        const int count =
            static_cast<int>(Backend::MAX_UNSENT_BYTES >> 20) + 16;
        for (int i = 0; i < count; ++i) {
            Server_->Bus().Publish("fixture.ping", payload);
        }

        QTRY_VERIFY_WITH_TIMEOUT(!crashed.isEmpty(), TIMEOUT_MS);
        QCOMPARE(crashed.at(0).at(0).toString(), reason);
        QVERIFY(WaitFor(stalled));
        QVERIFY(!stalled.result());
        QCOMPARE(Server_->Bus().SubscriptionCount(), before);

        // It is back, with nothing subscribed and nothing waiting for it.
        QTRY_VERIFY_WITH_TIMEOUT(RunInBackend("fixture.received").has_value(),
                                 TIMEOUT_MS);
        QVERIFY(BackendReceived().isEmpty());
        QCOMPARE(crashed.size(), 1);
    }

    void ABackendPublishesUnderItsOwnPrefix() {
        AddPlugin("one");
        QVERIFY(Start());
        QSignalSpy notified(Client_.get(), &RpcClient::NotificationReceived);
        const QFuture<rpc::Result> subscribed =
            Client_->Request("bus.subscribe", QJsonObject{{"pattern", "*"}});
        QVERIFY(WaitFor(subscribed) && subscribed.result());
        const int socketSubscription =
            subscribed.result()->toObject().value("subscription").toInt();
        const int own = BackendSubscribes(1, "one.*");

        const QString oversized(MAX_FRAME_LENGTH - 110, u'x');
        const QJsonObject huge = Notification(
            "bus.publish",
            QJsonObject{{"name", "one.big"}, {"payload", oversized}});
        QVERIFY(!Server::EventFitsAFrame("one.big", oversized));
        QVERIFY(QJsonDocument(huge).toJson(QJsonDocument::Compact).size() <
                Backend::MAX_LINE_LENGTH);

        QTest::ignoreMessage(
            QtWarningMsg,
            "roostd: the backend of one published event two.hello, which is "
            "not under one.");
        QTest::ignoreMessage(
            QtWarningMsg,
            "roostd: the backend of one published event oneother.hello, "
            "which is not under one.");
        QTest::ignoreMessage(
            QtWarningMsg,
            "roostd: the backend of one published an event without a valid "
            "name");
        QTest::ignoreMessage(
            QtWarningMsg,
            "roostd: the backend of one published an event without a valid "
            "name");
        QTest::ignoreMessage(
            QtWarningMsg,
            "roostd: the backend of one published event one.big, too big for "
            "a frame");
        BackendSends({
            Notification("bus.publish",
                         QJsonObject{{"name", "one.hello"},
                                     {"payload", QJsonObject{{"a", 1}}}}),
            Notification("bus.publish", QJsonObject{{"name", "two.hello"}}),
            Notification("bus.publish",
                         QJsonObject{{"name", "oneother.hello"}}),
            Notification("bus.publish", QJsonObject{{"name", "one..x"}}),
            Notification("bus.publish", QJsonObject{{"payload", 1}}),
            huge,
            // The barrier: it comes after everything that could have been
            // published.
            Notification("bus.publish", QJsonObject{{"name", "one.last"}}),
        });

        // Only the events this case publishes or tries to; a daemon event
        // under `*` is none of its business.
        const QStringList aboutThis{"one.hello", "two.hello", "oneother.hello",
                                    "one.big", "one.last"};
        auto events = [&] {
            QList<QJsonObject> list;
            for (const auto& call : notified) {
                const QJsonObject params =
                    call.at(1).value<QJsonValue>().toObject();
                if (call.at(0).toString() == "bus.event" &&
                    aboutThis.contains(params.value("name").toString())) {
                    list.append(params);
                }
            }
            return list;
        };
        auto names = [&] {
            QStringList list;
            for (const QJsonObject& event : events()) {
                list.append(event.value("name").toString());
            }
            return list;
        };
        QTRY_VERIFY_WITH_TIMEOUT(names().contains("one.last"), TIMEOUT_MS);
        QCOMPARE(names(), (QStringList{"one.hello", "one.last"}));
        QCOMPARE(events().at(0),
                 (QJsonObject{{"subscription", socketSubscription},
                              {"name", "one.hello"},
                              {"payload", QJsonObject{{"a", 1}}}}));

        // A backend subscribed to its own prefix gets its own events, with
        // a null payload for an absent one.
        const QJsonArray received =
            EventsNamed(BackendReceived(), {"one.hello", "one.last"});
        QCOMPARE(received.size(), 2);
        QCOMPARE(received[0].toObject(),
                 Event(own, "one.hello", QJsonObject{{"a", 1}}));
        QCOMPARE(received[1].toObject(), Event(own, "one.last", QJsonValue()));
    }

    void ABadSubscribeOrUnsubscribeIsAnErrorLikeTheSocketOnes() {
        AddPlugin("one");
        QVERIFY(Start());
        const QFuture<rpc::Result> subscribed =
            Client_->Request("bus.subscribe", QJsonObject{{"pattern", "x.*"}});
        QVERIFY(WaitFor(subscribed) && subscribed.result());
        const qint64 socketSubscription =
            subscribed.result()->toObject().value("subscription").toInteger();
        const std::size_t before = Server_->Bus().SubscriptionCount();

        struct Case {
            QString Method;
            QJsonValue Params;
        };
        const QList<Case> cases{
            {"bus.subscribe", QJsonObject{{"pattern", "a..b"}}},
            {"bus.subscribe", QJsonObject{{"pattern", 1}}},
            {"bus.subscribe", QJsonObject{}},
            {"bus.unsubscribe", QJsonObject{{"subscription", 99999}}},
            {"bus.unsubscribe", QJsonObject{{"subscription", "1"}}},
            {"bus.unsubscribe", QJsonObject{{"subscription", -1}}},
            // The socket connection's own, not the backend's.
            {"bus.unsubscribe",
             QJsonObject{{"subscription", socketSubscription}}},
        };
        QJsonArray requests;
        for (int i = 0; i < cases.size(); ++i) {
            requests.append(Request(i + 1, cases[i].Method, cases[i].Params));
        }
        requests.append(Request(100, "bus.nothing", QJsonObject{}));
        BackendSends(requests);
        const QJsonArray received = BackendReceived();

        // The socket connection answers the same to the same bad params.
        // The last case is not bad for it, as it is its own subscription.
        QString unsubscribeMessage;
        for (int i = 0; i < cases.size() - 1; ++i) {
            const QFuture<rpc::Result> expected =
                Client_->Request(cases[i].Method, cases[i].Params);
            QVERIFY(WaitFor(expected));
            QVERIFY(!expected.result());
            const QJsonObject error =
                AnswerTo(received, i + 1).value("error").toObject();
            QCOMPARE(error.value("code").toInt(),
                     static_cast<int>(rpc::EErrorCode::InvalidParams));
            QCOMPARE(error.value("code").toInt(),
                     expected.result().error().Code);
            QCOMPARE(error.value("message").toString(),
                     expected.result().error().Message);
            if (cases[i].Method == "bus.unsubscribe") {
                unsubscribeMessage = error.value("message").toString();
            }
        }
        // A subscription of another holder is unknown to the backend.
        const QJsonObject foreign =
            AnswerTo(received, int(cases.size())).value("error").toObject();
        QCOMPARE(foreign.value("code").toInt(),
                 static_cast<int>(rpc::EErrorCode::InvalidParams));
        QCOMPARE(foreign.value("message").toString(), unsubscribeMessage);
        QCOMPARE(Server_->Bus().SubscriptionCount(), before);

        const QJsonObject unknown =
            AnswerTo(received, 100).value("error").toObject();
        QCOMPARE(unknown.value("code").toInt(),
                 static_cast<int>(rpc::EErrorCode::MethodNotFound));
    }
};

QTEST_GUILESS_MAIN(PluginBusTest)
#include "plugin_bus_test.moc"
