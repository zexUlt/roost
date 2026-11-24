// Hiding and showing the lists of sessions: `layout.toggleLists`. The first
// half is the pure edit on trees (no window): the state is the one flag of the
// layout and no node changes, a window with only lists keeps them, the
// keyboard leaves a hidden list as it leaves a closed panel, a stack of tabs
// shows the next tab. The second half is the real window against a real
// roostd: a hidden list is collapsed and not closed, the sizes, the links and
// the selection come back with it, and the state comes back after a restart.

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlExtensionPlugin>
#include <QSignalSpy>
#include <QTest>

#include <optional>

#include "helpers/layout_window.hpp"
#include "helpers/scoped_env.hpp"
#include "roost/layout_commands.hpp"

using namespace roost;
using namespace roost::testing;

Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

LayoutKnownPanels Known() {
    return {
        {"sessions.list",
         {.Inputs = {}, .Outputs = {{"selected", "SessionRef"}}}},
        {"terminal.view",
         {.Inputs = {{"session", "SessionRef"}}, .Outputs = {}}},
    };
}

LayoutNode ListLeaf(const QString& instance) {
    LayoutNode node;
    node.Instance = instance;
    node.Panel = "sessions.list";
    return node;
}

LayoutNode TerminalLeaf(const QString& instance, const QString& list) {
    LayoutNode node;
    node.Instance = instance;
    node.Panel = "terminal.view";
    node.Links.insert("session",
                      {.From = list, .Output = "selected", .Problem = {}});
    return node;
}

LayoutNode Split(bool horizontal, std::vector<LayoutNode> children,
                 std::vector<LayoutSize> sizes) {
    LayoutNode node;
    node.Kind = ELayoutNodeKind::Split;
    node.Horizontal = horizontal;
    node.Children = std::move(children);
    node.Sizes = std::move(sizes);
    return node;
}

LayoutNode Tabs(std::vector<LayoutNode> children, int active) {
    LayoutNode node;
    node.Kind = ELayoutNodeKind::Tabs;
    node.Children = std::move(children);
    node.Active = active;
    return node;
}

// A list, the terminal it feeds and a second list, side by side.
Layout TwoListsAndATerminal() {
    return {.Root = Split(true,
                          {ListLeaf("list"), TerminalLeaf("terminal", "list"),
                           ListLeaf("list2")},
                          {LayoutSize::Px(320), LayoutSize::Rest(),
                           LayoutSize::Px(240)})};
}

Layout WithState(Layout layout, bool hidden) {
    layout.ListsHidden = hidden;
    return layout;
}

}  // namespace

class LayoutListsTest : public LayoutWindow {
    Q_OBJECT

private:
    std::optional<ScopedEnv> Home_;
    std::optional<ScopedEnv> Socket_;
    std::optional<ScopedEnv> Config_;

    // A document as text: a failed comparison shows both.
    static QByteArray Json(const QJsonObject& document) {
        return QJsonDocument(document).toJson(QJsonDocument::Compact);
    }

    // The events `store.changed` of the layout since the spy was made whose
    // document is `document`. A write of an earlier edit may still be on its
    // way when the spy is made: it has another document, and is not counted.
    static int WritesOf(const QSignalSpy& spy, const QByteArray& document) {
        int count = 0;
        for (const auto& call : spy) {
            if (call.at(0).toString() == QLatin1String("layout") &&
                Json(call.at(1).toJsonObject()) == document) {
                ++count;
            }
        }
        return count;
    }

    // The writes of the layout with the lists hidden (or shown).
    static int WritesWithState(const QSignalSpy& spy, bool hidden) {
        int count = 0;
        for (const auto& call : spy) {
            if (call.at(0).toString() == QLatin1String("layout") &&
                call.at(1).toJsonObject().value("listsHidden").toBool() ==
                    hidden) {
                ++count;
            }
        }
        return count;
    }

    void Focus(const QString& instance) {
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance(instance) != nullptr,
                                 TIMEOUT_MS);
        QQuickItem* slot = SlotByInstance(instance);
        slot->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(slot->hasActiveFocus(), TIMEOUT_MS);
    }

    // Returns when the daemon has answered a request made now: the events of
    // the requests before it have been delivered by then, so a write that was
    // under way is in the spies.
    void Barrier() {
        auto answer = Api_->Store().Get(QStringLiteral("layout"));
        QVERIFY(QTest::qWaitFor([&answer] { return answer.isFinished(); },
                                TIMEOUT_MS));
    }

    // Runs the command with the keyboard in `instance`; roostd writes the new
    // layout once, and what it keeps is the layout the window shows.
    void Toggle(const QString& instance) {
        Focus(instance);
        QVERIFY(Api_->Commands().HasLocal("layout.toggleLists"));
        const bool was = Shell_->CurrentLayout().ListsHidden;
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Api_->Commands().Run("layout.toggleLists");
        QTRY_COMPARE_WITH_TIMEOUT(Shell_->CurrentLayout().ListsHidden, !was,
                                  TIMEOUT_MS);
        const QByteArray expected = Json(LayoutToJson(Shell_->CurrentLayout()));
        QTRY_COMPARE_WITH_TIMEOUT(WritesOf(spy, expected), 1, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Json(StoredLayout()), expected, TIMEOUT_MS);
        // No second write of it follows.
        Barrier();
        QCOMPARE(WritesOf(spy, expected), 1);
    }

    bool HasProblem(const QString& part) {
        for (const QString& problem : Problems()) {
            if (problem.contains(part)) {
                return true;
            }
        }
        return false;
    }

    bool Shown(const QString& instance) {
        const QQuickItem* slot = SlotByInstance(instance);
        return slot != nullptr && slot->isVisible() && slot->width() > 1;
    }

    // The window property for the head of the terminal panel.
    bool ListsHiddenOfWindow() {
        return Root()->property("listsHidden").toBool();
    }

    void BootWithTwoLists() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Shell_->SetLayout(TwoListsAndATerminal());
        // Not settled: the terminal between two lists is narrower than the
        // 60 columns of the greeting.
        WaitForLayout({{"list", "terminal"}}, false);
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("list2") != nullptr,
                                 TIMEOUT_MS);
    }

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

    // ---- the pure edit ----------------------------------------------------

    void HidingIsTheStateOfTheLayoutAndNoNodeChanges() {
        const Layout before = TwoListsAndATerminal();
        const LayoutEdit hidden = ToggleLists(before, "terminal", Known());
        QVERIFY2(hidden.Refused.isEmpty(), qPrintable(hidden.Refused));
        QVERIFY(hidden.Changed);
        QVERIFY(hidden.Tree.ListsHidden);
        QCOMPARE(hidden.Tree.Root, before.Root);
        QCOMPARE(HiddenTypes(hidden.Tree, Known()),
                 (QSet<QString>{"sessions.list"}));
        QVERIFY(HiddenTypes(before, Known()).isEmpty());

        // And back: the tree is the one that was.
        const LayoutEdit shown = ToggleLists(hidden.Tree, "terminal", Known());
        QVERIFY2(shown.Refused.isEmpty(), qPrintable(shown.Refused));
        QVERIFY(shown.Changed);
        QCOMPARE(shown.Tree, before);
    }

    void ThePanelsOfTheListsAreCollapsedAndTheSplitHoldsTheRest() {
        const Layout hidden = WithState(TwoListsAndATerminal(), true);
        const QSet<QString> types = HiddenTypes(hidden, Known());
        QVERIFY(IsCollapsed(hidden.Root.Children[0], types));
        QVERIFY(!IsCollapsed(hidden.Root.Children[1], types));
        QVERIFY(IsCollapsed(hidden.Root.Children[2], types));
        QVERIFY(!IsCollapsed(hidden.Root, types));
        // Nothing is collapsed while the lists are shown.
        QVERIFY(!IsCollapsed(hidden.Root.Children[0], {}));
    }

    void HidingIsRefusedWhenOnlyListsWouldRemain() {
        const Layout lists = {
            .Root = Split(true, {ListLeaf("list"), ListLeaf("list2")},
                          {LayoutSize::Px(320), LayoutSize::Rest()})};
        const LayoutEdit edit = ToggleLists(lists, "list", Known());
        QVERIFY(edit.Refused.contains("only lists"));
        QVERIFY(!edit.Changed);
        QCOMPARE(edit.Tree, lists);
        QVERIFY(edit.Focus.isEmpty());
    }

    void HidingIsRefusedWhenThereIsNoList() {
        const Layout terminals = {
            .Root = Split(true,
                          {TerminalLeaf("terminal", "x"),
                           TerminalLeaf("terminal-2", "x")},
                          {LayoutSize::Px(320), LayoutSize::Rest()})};
        const LayoutEdit edit = ToggleLists(terminals, "terminal", Known());
        QVERIFY(edit.Refused.contains("no list"));
        QVERIFY(!edit.Changed);
        // A state left with no list in the window only goes.
        const LayoutEdit cleared =
            ToggleLists(WithState(terminals, true), "terminal", Known());
        QVERIFY(cleared.Refused.isEmpty());
        QVERIFY(!cleared.Tree.ListsHidden);
    }

    void TheKeyboardLeavesAHiddenListAsItLeavesAClosedPanel() {
        const Layout layout = TwoListsAndATerminal();
        // The list is the first: its neighbour after it, the terminal.
        QCOMPARE(ToggleLists(layout, "list", Known()).Focus,
                 QString("terminal"));
        // The second list: the neighbour before it.
        QCOMPARE(ToggleLists(layout, "list2", Known()).Focus,
                 QString("terminal"));
        // A panel that stays keeps it.
        QCOMPARE(ToggleLists(layout, "terminal", Known()).Focus,
                 QString("terminal"));
        // The neighbour is another list that goes too: the next one that shows.
        const Layout listsFirst = {
            .Root = Split(true,
                          {ListLeaf("list"), ListLeaf("list2"),
                           TerminalLeaf("terminal", "list")},
                          {LayoutSize::Px(320), LayoutSize::Px(240),
                           LayoutSize::Rest()})};
        QCOMPARE(ToggleLists(listsFirst, "list", Known()).Focus,
                 QString("terminal"));
        // A list in a split inside a split: the keyboard goes out of it.
        const Layout nested = {
            .Root = Split(true,
                          {Split(false, {ListLeaf("list"), ListLeaf("list2")},
                                 {LayoutSize::Px(200), LayoutSize::Rest()}),
                           TerminalLeaf("terminal", "list")},
                          {LayoutSize::Px(320), LayoutSize::Rest()})};
        QCOMPARE(ToggleLists(nested, "list2", Known()).Focus,
                 QString("terminal"));
        // Showing gives the keyboard to nobody else.
        QCOMPARE(
            ToggleLists(WithState(layout, true), "terminal", Known()).Focus,
            QString("terminal"));
    }

    void ATabStackShowsTheNextTabWhenTheShownOneIsAHiddenList() {
        const Layout stack = {
            .Root = Tabs({ListLeaf("list"), TerminalLeaf("terminal", "list"),
                          ListLeaf("list2")},
                         0)};
        const LayoutEdit edit = ToggleLists(stack, "list", Known());
        QVERIFY2(edit.Refused.isEmpty(), qPrintable(edit.Refused));
        // The stored tab stays: it is the one that comes back.
        QCOMPARE(edit.Tree.Root.Active, 0);
        const QSet<QString> types = HiddenTypes(edit.Tree, Known());
        QCOMPARE(ShownTab(edit.Tree.Root, types), 1);
        QCOMPARE(edit.Focus, QString("terminal"));
        // The last tab is a hidden list: the one before it.
        Layout last = stack;
        last.Root.Active = 2;
        QCOMPARE(ShownTab(last.Root, types), 1);
        // Shown lists change nothing.
        QCOMPARE(ShownTab(stack.Root, {}), 0);
        QCOMPARE(ShownTab(last.Root, {}), 2);
    }

    void TheStateIsAnOptionalFieldOfTheDocument() {
        const Layout hidden = WithState(TwoListsAndATerminal(), true);
        const QJsonObject document = LayoutToJson(hidden);
        QCOMPARE(document.value("version").toInt(), 1);
        QCOMPARE(document.value("listsHidden"), QJsonValue(true));
        const LayoutParseResult parsed = ParseLayout(document, Known());
        QVERIFY2(parsed.Problems.isEmpty(),
                 qPrintable(parsed.Problems.join(";")));
        QVERIFY(parsed.Writable);
        QCOMPARE(parsed.Tree, hidden);

        // Off, the key is not written, and a document without it is "shown".
        const QJsonObject off = LayoutToJson(TwoListsAndATerminal());
        QVERIFY(!off.contains("listsHidden"));
        QVERIFY(!ParseLayout(off, Known()).Tree.ListsHidden);

        // A value of another type is no reason to refuse the document.
        QJsonObject wrong = document;
        wrong.insert("listsHidden", "yes");
        const LayoutParseResult odd = ParseLayout(wrong, Known());
        QVERIFY(odd.Writable);
        QVERIFY(!odd.Tree.ListsHidden);
        QCOMPARE(odd.Tree.Root, hidden.Root);
        QCOMPARE(odd.Problems.size(), 1);
        QVERIFY(odd.Problems.first().contains("listsHidden"));
    }

    // ---- the window -------------------------------------------------------

    void HideAndShowKeepTheSizesTheLinksAndTheSelection() {
        BootWithTwoLists();
        if (QTest::currentTestFailed()) {
            return;
        }
        const QString id = IdIn("list", 1);
        OpenIdIn("list", "terminal", id);
        const QString id2 = IdIn("list2", 2);
        QVERIFY(id != id2);
        QQuickItem* list2 = PanelByInstance("list2");
        QVERIFY(list2 != nullptr);
        const QPointer<QQuickItem> list = PanelByInstance("list");
        const QPointer<QQuickItem> terminalPanel = PanelByInstance("terminal");

        const qreal listWidth = SlotByInstance("list")->width();
        const qreal list2Width = SlotByInstance("list2")->width();
        const qreal terminalWidth = SlotByInstance("terminal")->width();
        QCOMPARE(listWidth, 320.0);
        QCOMPARE(list2Width, 240.0);
        QVERIFY(!ListsHiddenOfWindow());

        Toggle("terminal");
        QVERIFY(Shell_->CurrentLayout().ListsHidden);
        QTRY_VERIFY_WITH_TIMEOUT(!SlotByInstance("list")->isVisible() &&
                                     !SlotByInstance("list2")->isVisible(),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ListsHiddenOfWindow(), TIMEOUT_MS);
        // Collapsed, not closed: the same panels, the same links, selection
        // and session, and the terminal has the room of the lists.
        QCOMPARE(SlotCount(), 3);
        QCOMPARE(PanelByInstance("list"), list.data());
        QCOMPARE(PanelByInstance("terminal"), terminalPanel.data());
        QCOMPARE(SelectedOf("list"), id);
        QCOMPARE(
            TerminalOf("terminal")->property("session").toMap().value("id"),
            QVariant(id));
        QCOMPARE(TerminalOf("terminal")->status(), QString("open"));
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal")->width() >=
                                     terminalWidth + listWidth + list2Width,
                                 TIMEOUT_MS);
        QVERIFY(Problems().isEmpty());

        Toggle("terminal");
        QVERIFY(!Shell_->CurrentLayout().ListsHidden);
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list") && Shown("list2"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(SlotByInstance("list")->width(), listWidth,
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(SlotByInstance("list2")->width(), list2Width,
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(SlotByInstance("terminal")->width(),
                                  terminalWidth, TIMEOUT_MS);
        QVERIFY(!ListsHiddenOfWindow());
        QCOMPARE(SelectedOf("list"), id);
        QCOMPARE(
            TerminalOf("terminal")->property("session").toMap().value("id"),
            QVariant(id));
        QCOMPARE(Shell_->CurrentLayout(), TwoListsAndATerminal());
        // The state is not in the document any more.
        QTRY_VERIFY_WITH_TIMEOUT(!StoredLayout().contains("listsHidden"),
                                 TIMEOUT_MS);
        QVERIFY(Problems().isEmpty());
    }

    void TheHiddenStateSurvivesARestartOfTheWindowAndRoostd() {
        BootWithTwoLists();
        if (QTest::currentTestFailed()) {
            return;
        }
        Toggle("terminal");
        QTRY_COMPARE_WITH_TIMEOUT(StoredLayout().value("listsHidden"),
                                  QJsonValue(true), TIMEOUT_MS);
        const Layout before = Shell_->CurrentLayout();

        CloseWindow();
        StopDaemon();
        QVERIFY(StartDaemon());
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Shell_->CurrentLayout(), before, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(SlotCount(), 3, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("list") != nullptr &&
                                     PanelByInstance("list2") != nullptr &&
                                     PanelByInstance("terminal") != nullptr,
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!SlotByInstance("list")->isVisible() &&
                                     !SlotByInstance("list2")->isVisible() &&
                                     Shown("terminal"),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ListsHiddenOfWindow(), TIMEOUT_MS);
        QVERIFY(Problems().isEmpty());
        // And the lists come back from there.
        Toggle("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list") && Shown("list2"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!StoredLayout().contains("listsHidden"),
                                 TIMEOUT_MS);
    }

    void AWindowWithOnlyListsKeepsThemAndTheStripSaysWhy() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        const Layout lists = {
            .Root = Split(true, {ListLeaf("list"), ListLeaf("list2")},
                          {LayoutSize::Px(320), LayoutSize::Rest()})};
        Shell_->SetLayout(lists);
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("list") != nullptr &&
                                     PanelByInstance("list2") != nullptr,
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list") && Shown("list2"), TIMEOUT_MS);
        Focus("list");
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Api_->Commands().Run("layout.toggleLists");
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem("only lists"), TIMEOUT_MS);
        QVERIFY(!Shell_->CurrentLayout().ListsHidden);
        QVERIFY(Shown("list") && Shown("list2"));
        QVERIFY(!ListsHiddenOfWindow());
        // The barrier: a write the command had made would be in the spy by
        // now, or answered after the request of the barrier.
        Barrier();
        QCOMPARE(WritesWithState(spy, true), 0);
        QCOMPARE(StoredLayout(), LayoutToJson(lists));
    }

    void TheKeyboardMovesOffAHiddenList() {
        BootWithTwoLists();
        if (QTest::currentTestFailed()) {
            return;
        }
        Toggle("list2");
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal")->hasActiveFocus(),
                                 TIMEOUT_MS);
        // Showing leaves it where it is.
        Toggle("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list2"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal")->hasActiveFocus(),
                                 TIMEOUT_MS);
        Toggle("list");
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal")->hasActiveFocus(),
                                 TIMEOUT_MS);
    }

    void ATabStackShowsTheNextTabAndTheListComesBack() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        const Layout stack = {
            .Root = Tabs({ListLeaf("list"), TerminalLeaf("terminal", "list"),
                          ListLeaf("list2")},
                         0)};
        Shell_->SetLayout(stack);
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("list") != nullptr &&
                                     PanelByInstance("terminal") != nullptr &&
                                     PanelByInstance("list2") != nullptr,
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list"), TIMEOUT_MS);
        QVERIFY(!SlotByInstance("terminal")->isVisible());

        Toggle("list");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("terminal"), TIMEOUT_MS);
        QVERIFY(!SlotByInstance("list")->isVisible());
        QVERIFY(!SlotByInstance("list2")->isVisible());
        QCOMPARE(SlotCount(), 3);
        // The tab the user had chosen is still the stored one.
        QCOMPARE(Shell_->CurrentLayout().Root.Active, 0);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal")->hasActiveFocus(),
                                 TIMEOUT_MS);

        Toggle("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list"), TIMEOUT_MS);
        QVERIFY(!SlotByInstance("terminal")->isVisible());
        QCOMPARE(Shell_->CurrentLayout(), stack);
        QVERIFY(Problems().isEmpty());
    }

    void ClosingTheLastPanelThatIsNotAListBringsTheListsBack() {
        BootWithTwoLists();
        if (QTest::currentTestFailed()) {
            return;
        }
        Toggle("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(!SlotByInstance("list")->isVisible(),
                                 TIMEOUT_MS);
        Focus("terminal");
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Api_->Commands().Run("layout.close");
        QTRY_COMPARE_WITH_TIMEOUT(SlotCount(), 2, TIMEOUT_MS);
        QVERIFY(!Shell_->CurrentLayout().ListsHidden);
        QTRY_COMPARE_WITH_TIMEOUT(
            WritesOf(spy, Json(LayoutToJson(Shell_->CurrentLayout()))), 1,
            TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list") && Shown("list2"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Json(StoredLayout()),
                                  Json(LayoutToJson(Shell_->CurrentLayout())),
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!StoredLayout().contains("listsHidden"),
                                 TIMEOUT_MS);
    }
};

QTEST_MAIN(LayoutListsTest)
#include "layout_lists_test.moc"
