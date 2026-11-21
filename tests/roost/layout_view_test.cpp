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

    // A stack of tabs, a pair on each, the one `active` shown.
    static Layout Tabbed(std::vector<LayoutNode> pairs, int active) {
        Layout layout;
        layout.Root.Kind = ELayoutNodeKind::Tabs;
        layout.Root.Active = active;
        layout.Root.Children = std::move(pairs);
        return layout;
    }

    QQuickItem* TabBarOf() {
        for (auto* item : Root()->findChildren<QQuickItem*>()) {
            if (item->inherits("QQuickTabBar")) {
                return item;
            }
        }
        return nullptr;
    }

    // A session of the list other than `id`.
    QString OtherThan(const QString& list, const QString& id) {
        for (int session = 0; session < 4; ++session) {
            if (IdIn(list, session) != id) {
                return IdIn(list, session);
            }
        }
        return {};
    }

    QString SessionIdOf(const QString& terminal) {
        return TerminalOf(terminal)
            ->property("session")
            .toMap()
            .value("id")
            .toString();
    }

    // Two terminals of two pairs show `shared`; the second owns it, the first
    // says it is elsewhere.
    void ShareASession(const QString& shared) {
        OpenIdIn("list", "terminal", shared);
        OpenIdIn("list2", "terminal2", shared);
        QTRY_COMPARE_WITH_TIMEOUT(TerminalText("terminal"), SHOWN_ELSEWHERE,
                                  TIMEOUT_MS);
        QVERIFY(TerminalOf("terminal")->superseded());
    }

    // The terminal shows `id` as its own, with no note.
    void VerifyShows(const QString& terminal, const QString& id) {
        QTRY_COMPARE_WITH_TIMEOUT(TerminalOf(terminal)->status(),
                                  QString("open"), TIMEOUT_MS);
        QVERIFY(!TerminalOf(terminal)->superseded());
        QVERIFY(TerminalText(terminal).isEmpty());
        QCOMPARE(SessionIdOf(terminal), id);
        QTRY_VERIFY2_WITH_TIMEOUT(ScreenOf(terminal).contains(Greeting(id)),
                                  qPrintable(ScreenOf(terminal)), TIMEOUT_MS);
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
        OpenIdIn("list", "terminal", first);
        QCOMPARE(TerminalOf("terminal")->status(), QString("open"));
        QCOMPARE(TerminalOf("terminal2")->status(), QString("none"));
        QVERIFY(SelectedOf("list2").isEmpty());
        QCOMPARE(TerminalText("terminal2"), EMPTY_TEXT);

        OpenIdIn("list2", "terminal2", second);
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
        OpenIdIn("list", "terminal", first);
        OpenIdIn("list2", "terminal2", second);
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
        OpenIdIn("list", "terminal", kept);
        OpenIdIn("list2", "terminal2", IdIn("list2", 1));
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
        OpenIdIn("list", "terminal", next);
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
        const QString shared = IdIn("list", 1);
        OpenIdIn("list", "terminal", shared);
        OpenIdIn("list2", "terminal2", shared);
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

    void ATerminalGetsTheSessionBackWhenTheOwnerSwitchesToAnother() {
        FailOnWarnings();
        BootTwoPairs();
        if (QTest::currentTestFailed()) {
            return;
        }
        const QString shared = IdIn("list", 1);
        ShareASession(shared);
        const QString other = OtherThan("list2", shared);
        OpenIdIn("list2", "terminal2", other);
        // Nobody shows `shared` now: the terminal that wants it does.
        VerifyShows("terminal", shared);
        VerifyShows("terminal2", other);
    }

    void ATerminalGetsTheSessionBackWhenThePairOfTheOwnerGoesAway() {
        FailOnWarnings();
        BootTwoPairs();
        if (QTest::currentTestFailed()) {
            return;
        }
        const QString shared = IdIn("list", 1);
        ShareASession(shared);
        Shell_->SetLayout(Stacked({Pair("list", "terminal")}));
        QTRY_COMPARE_WITH_TIMEOUT(SlotCount(), 2, TIMEOUT_MS);
        VerifyShows("terminal", shared);
    }

    // A new layout replaces the nodes, and the old ones are deleted only when
    // the event loop gets to them: the split must not lay them out beside the
    // new ones, or the new ones are squeezed to the minimum and a terminal in
    // one opens a session of a few columns.
    void ANewLayoutIsLaidOutWithoutTheNodesItReplaced() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Shell_->SetLayout(
            Stacked({Pair("list", "terminal"), Pair("list2", "terminal2")}));

        // Before the old nodes are deleted: the nodes of the new tree are the
        // ones shown, two pairs of two and the two that hold them.
        int shown = 0;
        for (auto* item : Root()->findChildren<QQuickItem*>("SplitNode")) {
            if (item->inherits("QQuickContentItem")) {
                for (auto* node : item->childItems()) {
                    shown += node->isVisible();
                }
            }
        }
        QCOMPARE(shown, 6);
        WaitForLayout({{"list", "terminal"}, {"list2", "terminal2"}});
    }

    void AHiddenTerminalDoesNotTakeTheSessionOfAVisibleOne() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Shell_->SetLayout(
            Tabbed({Pair("list", "terminal"), Pair("list2", "terminal2")}, 0));
        WaitForLayout({{"list", "terminal"}, {"list2", "terminal2"}}, false);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("list")->isVisible(),
                                 TIMEOUT_MS);
        QVERIFY(!SlotByInstance("list2")->isVisible());
        const QString shared = IdIn("list", 1);
        OpenIdIn("list", "terminal", shared);

        // The list on the hidden tab chooses the same session.
        QQuickItem* const hidden = PanelByInstance("list2");
        bool moved = false;
        QVERIFY(QMetaObject::invokeMethod(
            ModelOf("list2"), "setCursorRow", Q_RETURN_ARG(bool, moved),
            Q_ARG(int, RowOfId("list2", shared))));
        QVERIFY(moved);
        QVERIFY(QMetaObject::invokeMethod(hidden, "openCurrent"));
        QTRY_COMPARE_WITH_TIMEOUT(SelectedOf("list2"), shared, TIMEOUT_MS);
        // Its terminal has nothing to say until it is shown; a wait, as there
        // is nothing to wait for.
        QTest::qWait(300);
        VerifyShows("terminal", shared);
        QVERIFY(!TerminalOf("terminal2")->superseded());
        QCOMPARE(TerminalOf("terminal2")->status(), QString("none"));

        // The tab is shown: its terminal takes the session, and gives it back
        // when the tab goes.
        QQuickItem* const bar = TabBarOf();
        QVERIFY(bar != nullptr);
        bar->setProperty("currentIndex", 1);
        VerifyShows("terminal2", shared);
        QVERIFY(!TerminalOf("terminal")->superseded());
        bar->setProperty("currentIndex", 0);
        VerifyShows("terminal", shared);
        QVERIFY(!TerminalOf("terminal2")->superseded());
    }

    void FocusListSkipsTheListsOfTheTabsThatAreHidden() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Shell_->SetLayout(
            Tabbed({Pair("list", "terminal"), Pair("list2", "terminal2")}, 1));
        WaitForLayout({{"list", "terminal"}, {"list2", "terminal2"}}, false);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("list2")->isVisible(),
                                 TIMEOUT_MS);
        QVERIFY(!SlotByInstance("list")->isVisible());
        // The keyboard is in no panel: the command has no list to start from
        // but the first one of the layout that is shown.
        Window()->contentItem()->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(!SlotByInstance("list2")->hasActiveFocus() &&
                                     !SlotByInstance("list")->hasActiveFocus(),
                                 TIMEOUT_MS);
        const QFuture<rpc::Result> done =
            Api_->Commands().Run("shell.focusList");
        QTRY_VERIFY_WITH_TIMEOUT(done.isFinished(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ListViewOf("list2")->hasActiveFocus(),
                                 TIMEOUT_MS);
    }

    void StepsOfTheWidthDoNotCutTheListOffTheLayout() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        const auto step = [this](Qt::Key key) {
            ListViewOf("list")->forceActiveFocus();
            QTRY_VERIFY_WITH_TIMEOUT(ListViewOf("list")->hasActiveFocus(),
                                     TIMEOUT_MS);
            Press(key);
        };
        QCOMPARE(SlotByInstance("list")->width(), 320.0);
        step(Qt::Key_Less);
        QTRY_COMPARE_WITH_TIMEOUT(SlotByInstance("list")->width(), 300.0,
                                  TIMEOUT_MS);

        // A layout that is set after a step still sets the width.
        Layout wider;
        wider.Root = Pair("list", "terminal", 400);
        Shell_->SetLayout(wider);
        QTRY_COMPARE_WITH_TIMEOUT(SlotByInstance("list")->width(), 400.0,
                                  TIMEOUT_MS);

        // A saved width over the limit is not pulled to it by a step to the
        // narrower side.
        Layout wide;
        wide.Root = Pair("list", "terminal", 600);
        Shell_->SetLayout(wide);
        QTRY_COMPARE_WITH_TIMEOUT(SlotByInstance("list")->width(), 600.0,
                                  TIMEOUT_MS);
        step(Qt::Key_Less);
        QTRY_COMPARE_WITH_TIMEOUT(SlotByInstance("list")->width(), 580.0,
                                  TIMEOUT_MS);
    }
};

QTEST_MAIN(LayoutViewTest)
#include "layout_view_test.moc"
