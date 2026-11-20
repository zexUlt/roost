// The layout view of the window (LayoutView.qml) with the real roostd and
// two pairs of a list and a terminal: each list drives its own terminal, each
// keeps its own `opened`, a pair that leaves the layout takes its panels away
// and leaves the other pair connected, and a terminal that shows a session
// another terminal took over says so. The window, the daemon and the fake
// `claude` are helpers/sessions_window.hpp.

#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QQmlExtensionPlugin>
#include <QTest>

#include <optional>

#include "helpers/layout_window.hpp"
#include "helpers/scoped_env.hpp"

using namespace roost;
using namespace roost::testing;

Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

const QString SHOWN_ELSEWHERE = QStringLiteral("Shown in another panel.");

}  // namespace

class LayoutViewTest : public LayoutWindow {
    Q_OBJECT

private:
    // The default window with a second pair added through the model.
    void BootTwoPairs() {
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Shell_->SetLayout(
            Stacked({Pair("list", "terminal"), Pair("list2", "terminal2")}));
        WaitForLayout({{"list", "terminal"}, {"list2", "terminal2"}});
    }

    std::optional<ScopedEnv> Home_;
    std::optional<ScopedEnv> Socket_;
    std::optional<ScopedEnv> Config_;

private slots:
    void init() {
        Prepare();
        QVERIFY(QDir().mkpath(Dir_->filePath("home")));
        Home_.emplace("HOME", Dir_->filePath("home").toUtf8());
        Socket_.emplace("ROOST_SOCKET", SocketPath().toUtf8());
        Config_.emplace("ROOST_CONFIG", Dir_->filePath("config").toUtf8());
    }

    void cleanup() {
        Teardown();
        Config_.reset();
        Socket_.reset();
        Home_.reset();
    }

    void TheDefaultLayoutIsTheWindowOfBefore() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(SlotCount(), 2);
        QVERIFY(ListViewOf("list")->hasActiveFocus());
        QVERIFY(PanelByInstance("list") != nullptr);
        QVERIFY(PanelByInstance("terminal") != nullptr);
        QCOMPARE(Slot(LIST), SlotByInstance("list"));
        QCOMPARE(Slot(TERMINAL), SlotByInstance("terminal"));
        QCOMPARE(SlotByInstance("list")->width(), 320.0);
        OpenRow(1);
        QCOMPARE(
            TerminalOf("terminal")->property("session").toMap().value("id"),
            QVariant(SelectedId()));
    }

    void ASelectionInOneListChangesOnlyItsTerminal() {
        FailOnWarnings();
        BootTwoPairs();
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(SlotCount(), 4);
        const QString first = IdIn("list", 1);
        const QString second = IdIn("list2", 2);
        QVERIFY(first != second);
        OpenIn("list", "terminal", 1);
        QCOMPARE(TerminalOf("terminal")->status(), QString("open"));
        QCOMPARE(TerminalOf("terminal2")->status(), QString("none"));
        QVERIFY(SelectedOf("list2").isEmpty());
        QCOMPARE(TerminalText("terminal2"), EMPTY_TEXT);

        OpenIn("list2", "terminal2", 2);
        QCOMPARE(SelectedOf("list"), first);
        QCOMPARE(SelectedOf("list2"), second);
        QCOMPARE(
            TerminalOf("terminal")->property("session").toMap().value("id"),
            QVariant(first));
        QCOMPARE(
            TerminalOf("terminal2")->property("session").toMap().value("id"),
            QVariant(second));
        QCOMPARE(TerminalOf("terminal")->status(), QString("open"));
    }

    void EachListKeepsItsOwnOpenedSession() {
        FailOnWarnings();
        BootTwoPairs();
        if (QTest::currentTestFailed()) {
            return;
        }
        const QString first = IdIn("list", 1);
        const QString second = IdIn("list2", 3);
        QVERIFY(first != second);
        OpenIn("list", "terminal", 1);
        OpenIn("list2", "terminal2", 3);
        QTRY_VERIFY_WITH_TIMEOUT(OpenedStored("list").contains(first),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(OpenedStored("list2").contains(second),
                                 TIMEOUT_MS);
        QVERIFY(!OpenedStored("list").contains(second));
        QVERIFY(!OpenedStored("list2").contains(first));
    }

    void ClosingAPairLeavesTheOtherConnected() {
        FailOnWarnings();
        BootTwoPairs();
        if (QTest::currentTestFailed()) {
            return;
        }
        const QString kept = IdIn("list", 0);
        const QString next = IdIn("list", 2);
        OpenIn("list", "terminal", 0);
        OpenIn("list2", "terminal2", 1);
        QPointer<QQuickItem> gone = PanelByInstance("terminal2");
        QVERIFY(Api_->Commands().HasLocal("terminal.copy"));

        Shell_->SetLayout(Stacked({Pair("list", "terminal")}));
        QTRY_COMPARE_WITH_TIMEOUT(SlotCount(), 2, TIMEOUT_MS);
        QVERIFY(PanelByInstance("list2") == nullptr);
        QVERIFY(PanelByInstance("terminal2") == nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(gone.isNull(), TIMEOUT_MS);
        QCOMPARE(Root()->property("connection").toString(),
                 QString("connected"));
        QCOMPARE(TerminalOf("terminal")->status(), QString("open"));
        QCOMPARE(SelectedOf("list"), kept);
        // The pair that stays still follows its list.
        OpenIn("list", "terminal", 2);
        QCOMPARE(
            TerminalOf("terminal")->property("session").toMap().value("id"),
            QVariant(next));
        QVERIFY(Problems().isEmpty());
    }

    void ATerminalThatLostItsSessionSaysSoAndTakesItBack() {
        FailOnWarnings();
        BootTwoPairs();
        if (QTest::currentTestFailed()) {
            return;
        }
        // Both terminals show the same session: the list of the second pair
        // chooses what the first has.
        OpenIn("list", "terminal", 1);
        OpenIn("list2", "terminal2", 1);
        QTRY_COMPARE_WITH_TIMEOUT(TerminalText("terminal"), SHOWN_ELSEWHERE,
                                  TIMEOUT_MS);
        QVERIFY(TerminalOf("terminal")->superseded());
        QCOMPARE(TerminalOf("terminal")->status(), QString("none"));
        QVERIFY(!TerminalOf("terminal2")->superseded());
        QCOMPARE(TerminalOf("terminal2")->status(), QString("open"));
        QVERIFY(TerminalText("terminal2").isEmpty());

        // The focus takes it back, and the other one gives it up.
        TerminalOf("terminal")->forceActiveFocus();
        QTRY_COMPARE_WITH_TIMEOUT(TerminalText("terminal2"), SHOWN_ELSEWHERE,
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(TerminalOf("terminal")->status(),
                                  QString("open"), TIMEOUT_MS);
        QVERIFY(!TerminalOf("terminal")->superseded());
        QVERIFY(TerminalText("terminal").isEmpty());

        // Enter takes it back too, when the focus is already there.
        QVERIFY(TerminalOf("terminal")->hasActiveFocus());
        TerminalOf("terminal2")->takeBack();
        QTRY_COMPARE_WITH_TIMEOUT(TerminalText("terminal"), SHOWN_ELSEWHERE,
                                  TIMEOUT_MS);
        QVERIFY(TerminalOf("terminal")->hasActiveFocus());
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(TerminalOf("terminal")->status(),
                                  QString("open"), TIMEOUT_MS);
        QVERIFY(!TerminalOf("terminal")->superseded());
        QTRY_COMPARE_WITH_TIMEOUT(TerminalText("terminal2"), SHOWN_ELSEWHERE,
                                  TIMEOUT_MS);
    }
};

QTEST_MAIN(LayoutViewTest)
#include "layout_view_test.moc"
