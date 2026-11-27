// The Linux notifier: the arguments of org.freedesktop.Notifications.Notify,
// the choice of the icon, what it does when there is no bus, and the client
// path against a fake notification service on a private bus (a dbus-daemon of
// the test's own, never the session bus of the user): Notify and its id, the
// click (ActionInvoked), the close, the activation token and the restart of
// the service. The cases of the private bus are skipped where dbus-daemon is
// not on PATH. The notification on a real desktop and the focus of a Wayland
// compositor are checked by hand.

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSaveFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>

#include <memory>
#include <vector>

#include "roost/desktop_notifier_linux.hpp"

using namespace roost;

// The cases of the private bus need dbus-daemon; where it is not installed they
// are skipped, not failed.
#define START_BUS()                                                           \
    do {                                                                      \
        if (QStandardPaths::findExecutable("dbus-daemon").isEmpty()) {        \
            QSKIP("dbus-daemon is not on PATH");                              \
        }                                                                     \
        QVERIFY2(Start(), "the private bus and the fake service must start"); \
    } while (false)

namespace {

constexpr int TIMEOUT_MS = 10000;
const QString SERVICE = QStringLiteral("org.freedesktop.Notifications");
const QString PATH = QStringLiteral("/org/freedesktop/Notifications");
const char* const TOKEN_VARIABLE = "XDG_ACTIVATION_TOKEN";

// What a Notify call carried.
struct NotifyCall {
    QString AppName;
    uint ReplacesId = 0;
    QString Icon;
    QString Summary;
    QString Body;
    QStringList Actions;
    QVariantMap Hints;
    int ExpireTimeout = 0;
};

// The notification service of a desktop, as far as the notifier talks to it:
// it records Notify and answers with ids, and says what the test makes it
// emit.
class FakeNotifications : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")

public:
    std::vector<NotifyCall> Calls;

public slots:
    uint Notify(const QString& appName, uint replacesId, const QString& icon,
                const QString& summary, const QString& body,
                const QStringList& actions, const QVariantMap& hints,
                int expireTimeout) {
        Calls.push_back({appName, replacesId, icon, summary, body, actions,
                         hints, expireTimeout});
        return ++LastId_;
    }

signals:
    void ActionInvoked(uint id, const QString& action);
    void NotificationClosed(uint id, uint reason);
    void ActivationToken(uint id, const QString& token);

private:
    uint LastId_ = 100;
};

// A dbus-daemon of the test, with a configuration of its own: a socket in a
// temporary directory, no system services. Stopped by its PID at the end.
class PrivateBus {
public:
    ~PrivateBus() { Stop(); }

    bool Start() {
        if (!Dir_.isValid()) {
            return false;
        }
        QSaveFile config(Dir_.filePath("bus.conf"));
        if (!config.open(QIODevice::WriteOnly) ||
            config.write(QByteArray("<busconfig><type>session</type>"
                                    "<listen>unix:dir=") +
                         Dir_.path().toUtf8() +
                         "</listen><auth>EXTERNAL</auth>"
                         "<policy context=\"default\">"
                         "<allow send_destination=\"*\" eavesdrop=\"true\"/>"
                         "<allow eavesdrop=\"true\"/><allow own=\"*\"/>"
                         "</policy></busconfig>") < 0 ||
            !config.commit()) {
            return false;
        }
        Process_.start(QStandardPaths::findExecutable("dbus-daemon"),
                       {"--config-file=" + Dir_.filePath("bus.conf"),
                        "--nofork", "--print-address"});
        if (!Process_.waitForStarted(TIMEOUT_MS) ||
            !Process_.waitForReadyRead(TIMEOUT_MS)) {
            return false;
        }
        Address_ = QString::fromUtf8(Process_.readLine()).trimmed();
        return !Address_.isEmpty();
    }

    void Stop() {
        if (Process_.state() != QProcess::NotRunning) {
            // The PID of our own child.
            Process_.terminate();
            if (!Process_.waitForFinished(TIMEOUT_MS)) {
                Process_.kill();
                Process_.waitForFinished(TIMEOUT_MS);
            }
        }
    }

    const QString& Address() const { return Address_; }

private:
    QTemporaryDir Dir_;
    QProcess Process_;
    QString Address_;
};

}  // namespace

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
        QCOMPARE(arguments[6].toMap(),
                 (QVariantMap{{"desktop-entry", QStringLiteral("roost")}}));
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

private:
    // The private bus, the fake service on it and a connection for the
    // notifier. false: the bus cannot be started.
    bool Start() {
        qunsetenv(TOKEN_VARIABLE);
        if (!Bus_.Start()) {
            return false;
        }
        Fake_ = std::make_unique<FakeNotifications>();
        ServiceBus_ =
            QDBusConnection::connectToBus(Bus_.Address(), "roost-test-service");
        return ServiceBus_.isConnected() &&
               ServiceBus_.registerObject(
                   PATH, Fake_.get(),
                   QDBusConnection::ExportAllSlots |
                       QDBusConnection::ExportAllSignals) &&
               ServiceBus_.registerService(SERVICE);
    }

    QDBusConnection ClientBus() const {
        return QDBusConnection::connectToBus(Bus_.Address(),
                                             "roost-test-client");
    }

    // Shows one and waits until the fake service has it and the notifier
    // knows its id; the id of the service.
    uint ShowOne(LinuxDesktopNotifier& notifier, const QString& id) {
        const auto before = notifier.Open();
        const auto calls = Fake_->Calls.size();
        notifier.Show(id, "Title " + id, "Body " + id);
        if (!QTest::qWaitFor(
                [&] {
                    return notifier.Open() == before + 1 &&
                           Fake_->Calls.size() == calls + 1;
                },
                TIMEOUT_MS)) {
            return 0;
        }
        return 100 + static_cast<uint>(Fake_->Calls.size());
    }

private slots:
    void init() { Fake_.reset(); }

    void cleanup() {
        qunsetenv(TOKEN_VARIABLE);
        Fake_.reset();
        ServiceBus_ = QDBusConnection(QString());
        QDBusConnection::disconnectFromBus("roost-test-service");
        QDBusConnection::disconnectFromBus("roost-test-client");
        Bus_.Stop();
    }

private slots:
    void ShowCallsNotifyAndKeepsTheIdOfTheService() {
        START_BUS();
        LinuxDesktopNotifier notifier([this] { return ClientBus(); });
        QTest::failOnWarning();
        QCOMPARE_NE(ShowOne(notifier, "w1"), 0U);
        QCOMPARE(Fake_->Calls.size(), size_t{1});
        const NotifyCall& call = Fake_->Calls[0];
        QCOMPARE(call.AppName, "roost");
        QCOMPARE(call.ReplacesId, 0U);
        QCOMPARE(call.Icon,
                 NotificationIcon(QCoreApplication::applicationDirPath()));
        QCOMPARE(call.Summary, "Title w1");
        QCOMPARE(call.Body, "Body w1");
        QCOMPARE(call.Actions, (QStringList{"default", "Open"}));
        QCOMPARE(call.Hints,
                 (QVariantMap{{"desktop-entry", QStringLiteral("roost")}}));
        QCOMPARE(call.ExpireTimeout, -1);
    }

    void TheDefaultActionOfOurNotificationIsActivated() {
        START_BUS();
        LinuxDesktopNotifier notifier([this] { return ClientBus(); });
        QTest::failOnWarning();
        QSignalSpy activated(&notifier, &DesktopNotifier::Activated);
        const uint id = ShowOne(notifier, "w1");
        QVERIFY(id != 0);
        // Not ours, and not the default action: nothing. The one that is ours
        // comes after them on the same bus, so that it is the barrier.
        emit Fake_->ActionInvoked(id + 50, "default");
        emit Fake_->ActionInvoked(id, "other");
        emit Fake_->ActionInvoked(id, "default");
        QTRY_COMPARE_WITH_TIMEOUT(activated.size(), 1, TIMEOUT_MS);
        QCOMPARE(activated.at(0).at(0).toString(), "w1");
    }

    void AClosedNotificationIsForgotten() {
        START_BUS();
        LinuxDesktopNotifier notifier([this] { return ClientBus(); });
        QTest::failOnWarning();
        QSignalSpy activated(&notifier, &DesktopNotifier::Activated);
        const uint first = ShowOne(notifier, "w1");
        QVERIFY(first != 0);
        emit Fake_->NotificationClosed(first, 2);
        QTRY_COMPARE_WITH_TIMEOUT(notifier.Open(), qsizetype{0}, TIMEOUT_MS);
        // A click on the closed one does nothing; the second one, after it, is
        // the barrier.
        const uint second = ShowOne(notifier, "w2");
        QVERIFY(second != 0);
        emit Fake_->ActionInvoked(first, "default");
        emit Fake_->ActionInvoked(second, "default");
        QTRY_COMPARE_WITH_TIMEOUT(activated.size(), 1, TIMEOUT_MS);
        QCOMPARE(activated.at(0).at(0).toString(), "w2");
    }

    void TheActivationTokenOfOurNotificationIsSetForTheActivationOnly() {
        START_BUS();
        LinuxDesktopNotifier notifier([this] { return ClientBus(); });
        QTest::failOnWarning();
        // What the environment holds when the window is asked to take the
        // focus, which is the slot that Activated calls.
        QStringList seen;
        QObject::connect(&notifier, &DesktopNotifier::Activated, this,
                         [&seen](const QString& id) {
                             seen << id + "=" +
                                         qEnvironmentVariable(TOKEN_VARIABLE);
                         });
        const uint first = ShowOne(notifier, "w1");
        QVERIFY(first != 0);
        // A token of another notification is not ours.
        emit Fake_->ActivationToken(first + 50, "foreign");
        emit Fake_->ActionInvoked(first, "default");
        QTRY_COMPARE_WITH_TIMEOUT(seen, (QStringList{"w1="}), TIMEOUT_MS);

        const uint second = ShowOne(notifier, "w2");
        QVERIFY(second != 0);
        emit Fake_->ActivationToken(second, "token-2");
        emit Fake_->ActionInvoked(second, "default");
        QTRY_COMPARE_WITH_TIMEOUT(seen, (QStringList{"w1=", "w2=token-2"}),
                                  TIMEOUT_MS);
        // Not left for the next window that asks for the focus.
        QVERIFY(!qEnvironmentVariableIsSet(TOKEN_VARIABLE));
    }

    void TheIdsAreForgottenWhenTheServiceChangesItsOwner() {
        START_BUS();
        LinuxDesktopNotifier notifier([this] { return ClientBus(); });
        QTest::failOnWarning();
        QSignalSpy activated(&notifier, &DesktopNotifier::Activated);
        const uint old = ShowOne(notifier, "w1");
        QVERIFY(old != 0);
        // The service goes and comes back, as when the daemon of the desktop
        // is restarted.
        QVERIFY(ServiceBus_.unregisterService(SERVICE));
        QTRY_COMPARE_WITH_TIMEOUT(notifier.Open(), qsizetype{0}, TIMEOUT_MS);
        QVERIFY(ServiceBus_.registerService(SERVICE));
        const uint fresh = ShowOne(notifier, "w2");
        QVERIFY(fresh != 0);
        emit Fake_->ActionInvoked(old, "default");
        emit Fake_->ActionInvoked(fresh, "default");
        QTRY_COMPARE_WITH_TIMEOUT(activated.size(), 1, TIMEOUT_MS);
        QCOMPARE(activated.at(0).at(0).toString(), "w2");
    }

private:
    PrivateBus Bus_;
    QDBusConnection ServiceBus_{QString()};
    std::unique_ptr<FakeNotifications> Fake_;
};

QTEST_GUILESS_MAIN(LinuxNotifierTest)
#include "linux_notifier_test.moc"
