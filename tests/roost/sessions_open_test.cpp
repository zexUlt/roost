// The click on a desktop notification in the real window: the window of the
// shell with the sessions panel and the terminal against the real roostd with
// the real notifications plugin and the real Claude plugin behind a fake
// `claude`; a session that turns blocked is shown to a notifier of the test,
// which then clicks it. What is checked is what the window does with the click
// (the session is open as with Enter on its row), not the notification of the
// system. The window is helpers/sessions_window.hpp.

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlExtensionPlugin>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTest>

#include "helpers/recording_notifier.hpp"
#include "helpers/sessions_window.hpp"

using namespace roost;
using namespace roost::testing;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

class SessionsOpenTest : public SessionsWindow {
    Q_OBJECT

private:
    // `claude agents` lists the sessions of the fixture, the first of them
    // blocked (it was working): the adapter reports the change, and the
    // notifications plugin publishes it.
    bool BlockTheFirst() const {
        QJsonArray rows =
            QJsonDocument::fromJson(
                ReadFile(QStringLiteral(FIXTURES_DIR "/claude_agents.json")))
                .array();
        QJsonObject first = rows.at(0).toObject();
        first.insert("state", "blocked");
        rows.replace(0, first);
        QSaveFile agents(AgentsPath());
        return agents.open(QIODevice::WriteOnly) &&
               agents.write(
                   QJsonDocument(rows).toJson(QJsonDocument::Compact)) >= 0 &&
               agents.commit();
    }

    // The session the terminal panel is given by the link, "<agent>/<id>".
    QString TerminalSession() {
        const QVariantMap ref = PanelOf(TERMINAL)->property("session").toMap();
        return ref.isEmpty() ? QString()
                             : ref.value("agent").toString() + "/" +
                                   ref.value("id").toString();
    }

private slots:
    void init() { Prepare(); }

    void cleanup() { Teardown(); }

    void AClickOnTheNotificationOpensItsSessionAsEnterDoes() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        RecordingNotifier notifier;
        const WindowNotifications& notifications =
            AttachNotifications(notifier);
        QTRY_VERIFY_WITH_TIMEOUT(notifications.IsLastFocused(), TIMEOUT_MS);
        QVERIFY(SelectedId().isEmpty());
        QVERIFY(TerminalSession().isEmpty());

        QVERIFY(BlockTheFirst());
        QTRY_COMPARE_WITH_TIMEOUT(notifier.Calls.size(), size_t{1}, TIMEOUT_MS);
        QCOMPARE(notifier.Calls[0].Title, "Fix the flaky test");
        QCOMPARE(notifier.Calls[0].Body, "needs you");

        // The click raises the window; the offscreen platform cannot, and
        // says so.
        QTest::ignoreMessage(QtWarningMsg,
                             "This plugin does not support raise()");
        notifier.Click(notifier.Calls[0].Id);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QCOMPARE(SelectedId(), ALPHA);
        QCOMPARE(TerminalSession(), "claude/" + ALPHA);
        Settle();
        QCOMPARE(ScreenNow().contains(Greeting(ALPHA)), true);
    }

    void AClickOnTheTestNotificationOnlyRaisesTheWindow() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        RecordingNotifier notifier;
        const WindowNotifications& notifications =
            AttachNotifications(notifier);
        QTRY_VERIFY_WITH_TIMEOUT(notifications.IsLastFocused(), TIMEOUT_MS);

        const QFuture<rpc::Result> ran =
            Api_->Commands().Run("notifications.test", QJsonObject{});
        QTRY_VERIFY_WITH_TIMEOUT(ran.isFinished(), TIMEOUT_MS);
        QVERIFY(ran.result().has_value());
        QTRY_COMPARE_WITH_TIMEOUT(notifier.Calls.size(), size_t{1}, TIMEOUT_MS);

        QTest::ignoreMessage(QtWarningMsg,
                             "This plugin does not support raise()");
        notifier.Click(notifier.Calls[0].Id);
        WAIT_FOR_EFFECTS();
        QVERIFY(SelectedId().isEmpty());
        QVERIFY(TerminalSession().isEmpty());
        QCOMPARE(Status(), QString("none"));
    }

    void AnUnknownSessionWarnsAndChangesNothing() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        const QString before = TerminalSession();
        QVERIFY(!before.isEmpty());

        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression("sessions\\.open: no session claude/nope"));
        const QFuture<rpc::Result> ran = Api_->Commands().Run(
            "sessions.open", QJsonObject{{"agent", "claude"}, {"id", "nope"}});
        QTRY_VERIFY_WITH_TIMEOUT(ran.isFinished(), TIMEOUT_MS);
        QVERIFY(ran.result().has_value());
        WAIT_FOR_EFFECTS();
        QCOMPARE(SelectedId(), IdAt(1));
        QCOMPARE(TerminalSession(), before);
        QCOMPARE(Status(), QString("open"));
    }
};

QTEST_MAIN(SessionsOpenTest)
#include "sessions_open_test.moc"
