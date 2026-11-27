// The identifiers of the requests of the macOS notification center: which
// click is ours. Plain functions, built and run on every platform.

#include <QTest>

#include "roost/notification_ids.hpp"

using namespace roost;

class NotificationIdsTest : public QObject {
    Q_OBJECT

private slots:
    void IdentifierNamesTheTokenAndTheId();
    void ActivatedIdIsReturnedForOurOwnIdentifier();
    void ActivatedIdKeepsDashesOfTheId();
    void ActivatedIdIsNothingForAnotherToken();
    void ActivatedIdIsNothingForAnotherKind();
    void ActivatedIdIsNothingWhenTheUserInfoDisagrees();
};

void NotificationIdsTest::IdentifierNamesTheTokenAndTheId() {
    QCOMPARE(
        NotificationIdentifier(QStringLiteral("0f3a"), QStringLiteral("n-7")),
        QStringLiteral("roost-0f3a-n-7"));
}

void NotificationIdsTest::ActivatedIdIsReturnedForOurOwnIdentifier() {
    const QString token = QStringLiteral("0f3a");
    const auto id = ActivatedNotificationId(
        NotificationIdentifier(token, QStringLiteral("a")), QStringLiteral("a"),
        token);
    QVERIFY(id.has_value());
    QCOMPARE(*id, QStringLiteral("a"));
}

void NotificationIdsTest::ActivatedIdKeepsDashesOfTheId() {
    const QString token = QStringLiteral("0f3a");
    const QString id = QStringLiteral("claude/bg-0a1b-2c");
    const auto activated =
        ActivatedNotificationId(NotificationIdentifier(token, id), id, token);
    QVERIFY(activated.has_value());
    QCOMPARE(*activated, id);
}

void NotificationIdsTest::ActivatedIdIsNothingForAnotherToken() {
    // The notification of an earlier run, or of another roost.
    const QString identifier =
        NotificationIdentifier(QStringLiteral("0f3a"), QStringLiteral("a"));
    QVERIFY(!ActivatedNotificationId(identifier, QStringLiteral("a"),
                                     QStringLiteral("9c1d"))
                 .has_value());
    // A token that is a prefix of ours is not ours.
    QVERIFY(!ActivatedNotificationId(identifier, QStringLiteral("a"),
                                     QStringLiteral("0f"))
                 .has_value());
    // Nor is no token at all.
    QVERIFY(!ActivatedNotificationId(identifier, QStringLiteral("a"), QString())
                 .has_value());
}

void NotificationIdsTest::ActivatedIdIsNothingForAnotherKind() {
    const QString token = QStringLiteral("0f3a");
    QVERIFY(!ActivatedNotificationId(QStringLiteral("other-0f3a-a"),
                                     QStringLiteral("a"), token)
                 .has_value());
    QVERIFY(!ActivatedNotificationId(QString(), QString(), token).has_value());
    // No id at all.
    QVERIFY(!ActivatedNotificationId(QStringLiteral("roost-0f3a-"), QString(),
                                     token)
                 .has_value());
}

void NotificationIdsTest::ActivatedIdIsNothingWhenTheUserInfoDisagrees() {
    const QString token = QStringLiteral("0f3a");
    QVERIFY(!ActivatedNotificationId(
                 NotificationIdentifier(token, QStringLiteral("a")),
                 QStringLiteral("b"), token)
                 .has_value());
}

QTEST_GUILESS_MAIN(NotificationIdsTest)
#include "notification_ids_test.moc"
