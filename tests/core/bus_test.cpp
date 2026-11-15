// The event bus: which subscriptions an event reaches, in what order, and what
// a listener may do to the bus while it is being called.

#include <QJsonObject>
#include <QList>
#include <QStringList>
#include <QTest>

#include <memory>
#include <optional>

#include "core/bus.hpp"

using roost::EventBus;

namespace {

// Subscribes a listener that appends "<tag>:<event name>" to a shared log.
EventBus::SubscriptionId Record(EventBus& bus, const QString& pattern,
                                const QString& tag, QStringList& log) {
    const std::optional<EventBus::SubscriptionId> id = bus.Subscribe(
        pattern, [&log, tag](const QString& name, const QJsonValue&) {
            log.append(tag + u':' + name);
        });
    if (!id) {
        qFatal("could not subscribe to %s", qPrintable(pattern));
    }
    return *id;
}

}  // namespace

class BusTest : public QObject {
    Q_OBJECT

private slots:
    void Matching_data();
    void Matching();
    void InvalidPatternsAreRefused_data();
    void InvalidPatternsAreRefused();
    void InvalidNamesAreNotPublished();
    void PayloadReachesTheListener();
    void DeliveryFollowsSubscriptionOrder();
    void Unsubscribe();
    void ListenerUnsubscribesItself();
    void ListenerUnsubscribesALaterOne();
    void ListenerSubscribesAnother();
    void ListenerPublishes();
};

void BusTest::Matching_data() {
    QTest::addColumn<QString>("pattern");
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("matches");

    QTest::newRow("exact") << "session.opened" << "session.opened" << true;
    QTest::newRow("exact, other name")
        << "session.opened" << "session.closed" << false;
    QTest::newRow("exact is not a prefix")
        << "session" << "session.opened" << false;
    QTest::newRow("prefix") << "session.*" << "session.opened" << true;
    QTest::newRow("prefix, deeper") << "session.*" << "session.a.b" << true;
    QTest::newRow("prefix, not itself") << "session.*" << "session" << false;
    QTest::newRow("prefix, longer word")
        << "session.*" << "sessions.opened" << false;
    QTest::newRow("prefix, other") << "session.*" << "plugin.loaded" << false;
    QTest::newRow("nested prefix") << "a.b.*" << "a.b.c" << true;
    QTest::newRow("nested prefix, parent") << "a.b.*" << "a.c" << false;
    QTest::newRow("everything") << "*" << "x" << true;
    QTest::newRow("everything, dotted") << "*" << "a.b.c" << true;
    QTest::newRow("case matters") << "Session.*" << "session.x" << false;
}

void BusTest::Matching() {
    QFETCH(QString, pattern);
    QFETCH(QString, name);
    QFETCH(bool, matches);

    EventBus bus;
    QStringList log;
    Record(bus, pattern, QStringLiteral("s"), log);
    QVERIFY(bus.Publish(name, QJsonValue::Null));
    QCOMPARE(!log.isEmpty(), matches);
}

void BusTest::InvalidPatternsAreRefused_data() {
    QTest::addColumn<QString>("pattern");

    for (const char* pattern :
         {"", ".", "session.", ".session", "a..b", "a b", "a/b", "a*", "*.a",
          "a.*.b", "a.**", "**", ".*", "сессия", "a.*b"}) {
        QTest::newRow(pattern) << QString::fromUtf8(pattern);
    }
}

void BusTest::InvalidPatternsAreRefused() {
    QFETCH(QString, pattern);
    EventBus bus;
    QVERIFY(!bus.Subscribe(pattern, [](const QString&, const QJsonValue&) {}));
}

void BusTest::InvalidNamesAreNotPublished() {
    EventBus bus;
    QStringList log;
    Record(bus, QStringLiteral("*"), QStringLiteral("all"), log);
    for (const char* name : {"", "*", "a.*", "a..b", "a.", "a b"}) {
        QVERIFY2(!bus.Publish(QString::fromUtf8(name), QJsonValue::Null), name);
    }
    QVERIFY(log.isEmpty());
    QVERIFY(!bus.Subscribe(QStringLiteral("*"), EventBus::Listener()));
}

void BusTest::PayloadReachesTheListener() {
    EventBus bus;
    QJsonValue seen;
    QVERIFY(bus.Subscribe(QStringLiteral("a"),
                          [&seen](const QString&, const QJsonValue& payload) {
                              seen = payload;
                          }));
    const QJsonObject payload{{QStringLiteral("id"), 7}};
    QVERIFY(bus.Publish(QStringLiteral("a"), payload));
    QCOMPARE(seen, QJsonValue(payload));
}

void BusTest::DeliveryFollowsSubscriptionOrder() {
    EventBus bus;
    QStringList log;
    Record(bus, QStringLiteral("*"), QStringLiteral("1"), log);
    Record(bus, QStringLiteral("session.opened"), QStringLiteral("2"), log);
    Record(bus, QStringLiteral("session.*"), QStringLiteral("3"), log);
    Record(bus, QStringLiteral("plugin.*"), QStringLiteral("4"), log);

    QVERIFY(bus.Publish(QStringLiteral("session.opened"), QJsonValue::Null));
    QCOMPARE(log, QStringList({"1:session.opened", "2:session.opened",
                               "3:session.opened"}));
}

void BusTest::Unsubscribe() {
    EventBus bus;
    QStringList log;
    const auto id = Record(bus, QStringLiteral("a"), QStringLiteral("s"), log);
    QVERIFY(bus.Unsubscribe(id));
    QVERIFY(!bus.Unsubscribe(id));
    QVERIFY(!bus.Unsubscribe(id + 100));
    QVERIFY(bus.Publish(QStringLiteral("a"), QJsonValue::Null));
    QVERIFY(log.isEmpty());
}

void BusTest::ListenerUnsubscribesItself() {
    EventBus bus;
    QStringList log;
    // The listener owns state it touches after unsubscribing: under ASan this
    // catches a listener destroyed while it runs.
    auto self = std::make_shared<EventBus::SubscriptionId>(0);
    *self = *bus.Subscribe(QStringLiteral("a"),
                           [&bus, &log, self, tag = QStringLiteral("once")](
                               const QString& name, const QJsonValue&) {
                               QVERIFY(bus.Unsubscribe(*self));
                               log.append(tag + u':' + name);
                           });
    Record(bus, QStringLiteral("a"), QStringLiteral("after"), log);

    QVERIFY(bus.Publish(QStringLiteral("a"), QJsonValue::Null));
    QVERIFY(bus.Publish(QStringLiteral("a"), QJsonValue::Null));
    QCOMPARE(log, QStringList({"once:a", "after:a", "after:a"}));
}

void BusTest::ListenerUnsubscribesALaterOne() {
    EventBus bus;
    QStringList log;
    EventBus::SubscriptionId later = 0;
    QVERIFY(bus.Subscribe(QStringLiteral("a"),
                          [&](const QString&, const QJsonValue&) {
                              log.append(QStringLiteral("first"));
                              bus.Unsubscribe(later);
                          }));
    later = Record(bus, QStringLiteral("a"), QStringLiteral("later"), log);
    Record(bus, QStringLiteral("a"), QStringLiteral("last"), log);

    QVERIFY(bus.Publish(QStringLiteral("a"), QJsonValue::Null));
    QCOMPARE(log, QStringList({"first", "last:a"}));
}

void BusTest::ListenerSubscribesAnother() {
    EventBus bus;
    QStringList log;
    bool subscribed = false;
    QVERIFY(bus.Subscribe(
        QStringLiteral("a"), [&](const QString&, const QJsonValue&) {
            if (!subscribed) {
                subscribed = true;
                Record(bus, QStringLiteral("a"), QStringLiteral("new"), log);
            }
        }));

    QVERIFY(bus.Publish(QStringLiteral("a"), QJsonValue::Null));
    QVERIFY(log.isEmpty());
    QVERIFY(bus.Publish(QStringLiteral("a"), QJsonValue::Null));
    QCOMPARE(log, QStringList({"new:a"}));
}

void BusTest::ListenerPublishes() {
    EventBus bus;
    QStringList log;
    Record(bus, QStringLiteral("*"), QStringLiteral("1"), log);
    QVERIFY(bus.Subscribe(
        QStringLiteral("outer"), [&](const QString&, const QJsonValue&) {
            bus.Publish(QStringLiteral("inner"), QJsonValue::Null);
        }));
    Record(bus, QStringLiteral("*"), QStringLiteral("3"), log);

    QVERIFY(bus.Publish(QStringLiteral("outer"), QJsonValue::Null));
    QCOMPARE(log, QStringList({"1:outer", "1:inner", "3:inner", "3:outer"}));
}

QTEST_GUILESS_MAIN(BusTest)
#include "bus_test.moc"
