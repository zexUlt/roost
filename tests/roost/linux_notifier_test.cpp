// The part of the Linux notifier that needs no session bus: the arguments of
// org.freedesktop.Notifications.Notify, the choice of the icon, and what it
// does when there is no bus. The rest (the notification on a real desktop, the
// click) is checked by hand.

#include <QDBusConnection>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>

#include "roost/desktop_notifier_linux.hpp"

using namespace roost;

class LinuxNotifierTest : public QObject {
    Q_OBJECT

private slots:
    void TheArgumentsAreThoseOfTheNotifyMethod() {
        const QVariantList arguments =
            NotifyArguments("Fix the build", "needs you", "/icons/roost.png");
        QCOMPARE(arguments.size(), 8);
        // app_name, replaces_id, app_icon, summary, body, actions, hints,
        // expire_timeout: the types are those of the signature "susssasa{sv}i".
        QCOMPARE(arguments[0], QVariant(QStringLiteral("roost")));
        QCOMPARE(arguments[1].metaType(), QMetaType::fromType<quint32>());
        QCOMPARE(arguments[1].value<quint32>(), 0U);
        QCOMPARE(arguments[2], QVariant(QStringLiteral("/icons/roost.png")));
        QCOMPARE(arguments[3], QVariant(QStringLiteral("Fix the build")));
        QCOMPARE(arguments[4], QVariant(QStringLiteral("needs you")));
        QCOMPARE(arguments[5].metaType(), QMetaType::fromType<QStringList>());
        QCOMPARE(arguments[5].toStringList(), (QStringList{"default", "Open"}));
        QCOMPARE(arguments[6].metaType(), QMetaType::fromType<QVariantMap>());
        QVERIFY(arguments[6].toMap().isEmpty());
        QCOMPARE(arguments[7].metaType(), QMetaType::fromType<qint32>());
        QCOMPARE(arguments[7].value<qint32>(), -1);
    }

    void TheIconIsThe128PxPngNextToTheBinaryIfItIsThere() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(QDir(root.path()).mkpath("bin"));
        const QString bin = root.filePath("bin");
        // The install layout: bin/ next to share/icons/hicolor.
        QCOMPARE(NotificationIcon(bin), QStringLiteral("roost"));
        const QString dir = root.filePath("share/icons/hicolor/128x128/apps");
        QVERIFY(QDir(root.path()).mkpath("share/icons/hicolor/128x128/apps"));
        // Another size is not the one.
        QVERIFY(QDir(root.path()).mkpath("share/icons/hicolor/256x256/apps"));
        QFile other(
            root.filePath("share/icons/hicolor/256x256/apps/roost.png"));
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.close();
        QCOMPARE(NotificationIcon(bin), QStringLiteral("roost"));
        QFile icon(dir + "/roost.png");
        QVERIFY(icon.open(QIODevice::WriteOnly));
        icon.close();
        // The absolute path, with no "..": what the server needs to open it.
        const QString expected =
            QFileInfo(dir + "/roost.png").canonicalFilePath();
        QCOMPARE(NotificationIcon(bin), expected);
        QVERIFY(QDir::isAbsolutePath(expected));
        QVERIFY(!expected.contains(".."));
    }

    void WithNoSessionBusItWarnsOnceAndShowsNothing() {
        // A connection of a name nobody opened is not connected.
        LinuxDesktopNotifier notifier(
            [] { return QDBusConnection(QStringLiteral("roost-no-bus")); });
        QSignalSpy activated(&notifier, &DesktopNotifier::Activated);
        QTest::ignoreMessage(QtWarningMsg,
                             "roost: no desktop notifications: no session bus");
        notifier.Show("1", "title", "body");
        // A second one warns no more, and the test fails on a warning.
        QTest::failOnWarning();
        notifier.Show("2", "title", "body");
        QCOMPARE(notifier.Open(), 0);
        QVERIFY(activated.isEmpty());
    }
};

QTEST_GUILESS_MAIN(LinuxNotifierTest)
#include "linux_notifier_test.moc"
