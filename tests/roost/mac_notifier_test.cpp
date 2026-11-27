// The macOS notifier in a process with no application bundle (this test
// binary: the center throws for such a process, so the notifier must never ask
// it). A notification on screen needs roost.app and the user's permission and
// is checked by hand. The identifiers: notification_ids_test.

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include "roost/desktop_notifier_macos.hpp"

using namespace roost;

class MacNotifierTest : public QObject {
    Q_OBJECT

private slots:
    void WithoutABundleShowWarnsOnceAndShowsNothing();
};

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
