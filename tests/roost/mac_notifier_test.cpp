// The macOS notifier: the identifiers of the requests it gives the
// notification center, and what it does in a process with no application
// bundle (this test binary: the center throws for such a process, so the
// notifier must never ask it). A notification on screen needs roost.app and
// the user's permission and is checked by hand.

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include "roost/desktop_notifier_macos.hpp"

using namespace roost;

class MacNotifierTest : public QObject {
    Q_OBJECT

private slots:
    void IdentifierNamesThePidAndTheId();
    void ActivatedIdIsReturnedForOurOwnIdentifier();
    void ActivatedIdKeepsDashesOfTheId();
    void ActivatedIdIsNothingForAnotherPid();
    void ActivatedIdIsNothingForAnotherKind();
    void ActivatedIdIsNothingWhenTheUserInfoDisagrees();
    void WithoutABundleShowWarnsOnceAndShowsNothing();
};

void MacNotifierTest::IdentifierNamesThePidAndTheId() {
    QCOMPARE(NotificationIdentifier(4242, QStringLiteral("n-7")),
             QStringLiteral("roost-4242-n-7"));
}

void MacNotifierTest::ActivatedIdIsReturnedForOurOwnIdentifier() {
    const QString identifier =
        NotificationIdentifier(4242, QStringLiteral("a"));
    const auto id =
        ActivatedNotificationId(identifier, QStringLiteral("a"), 4242);
    QVERIFY(id.has_value());
    QCOMPARE(*id, QStringLiteral("a"));
}

void MacNotifierTest::ActivatedIdKeepsDashesOfTheId() {
    const QString id = QStringLiteral("claude/bg-0a1b-2c");
    const auto activated =
        ActivatedNotificationId(NotificationIdentifier(7, id), id, 7);
    QVERIFY(activated.has_value());
    QCOMPARE(*activated, id);
}

void MacNotifierTest::ActivatedIdIsNothingForAnotherPid() {
    // The notification of an earlier run, still in the Notification Center.
    QVERIFY(!ActivatedNotificationId(
                 NotificationIdentifier(4242, QStringLiteral("a")),
                 QStringLiteral("a"), 4243)
                 .has_value());
    // 42 is a prefix of 4242, and not the same pid.
    QVERIFY(!ActivatedNotificationId(
                 NotificationIdentifier(4242, QStringLiteral("a")),
                 QStringLiteral("a"), 42)
                 .has_value());
}

void MacNotifierTest::ActivatedIdIsNothingForAnotherKind() {
    QVERIFY(!ActivatedNotificationId(QStringLiteral("other-4242-a"),
                                     QStringLiteral("a"), 4242)
                 .has_value());
    QVERIFY(!ActivatedNotificationId(QString(), QString(), 4242).has_value());
    // No id at all.
    QVERIFY(
        !ActivatedNotificationId(QStringLiteral("roost-4242-"), QString(), 4242)
             .has_value());
}

void MacNotifierTest::ActivatedIdIsNothingWhenTheUserInfoDisagrees() {
    QVERIFY(!ActivatedNotificationId(
                 NotificationIdentifier(4242, QStringLiteral("a")),
                 QStringLiteral("b"), 4242)
                 .has_value());
}

void MacNotifierTest::WithoutABundleShowWarnsOnceAndShowsNothing() {
    MacDesktopNotifier notifier;
    QSignalSpy activated(&notifier, &DesktopNotifier::Activated);

    QTest::ignoreMessage(QtWarningMsg,
                         "roost: desktop notifications need roost.app");
    notifier.Show(QStringLiteral("a"), QStringLiteral("title"),
                  QStringLiteral("body"));

    // The second Show warns about nothing: any warning fails the case now.
    QTest::failOnWarning();
    notifier.Show(QStringLiteral("b"), QStringLiteral("title"),
                  QStringLiteral("body"));

    QCOMPARE(activated.count(), 0);
}

QTEST_GUILESS_MAIN(MacNotifierTest)
#include "mac_notifier_test.moc"
