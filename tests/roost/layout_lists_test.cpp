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
#include <QQuickItemGrabResult>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
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

// Two terminals between two lists, the terminals sharing the room.
Layout TerminalsBetweenLists() {
    return {.Root =
                Split(true,
                      {ListLeaf("list"), TerminalLeaf("terminal", "list"),
                       TerminalLeaf("terminal-2", "list"), ListLeaf("list2")},
                      {LayoutSize::Px(320), LayoutSize::Rest(),
                       LayoutSize::Rest(), LayoutSize::Px(240)})};
}

// The same, the first list taking the room and the terminals in pixels.
Layout FillListFirst() {
    return {.Root =
                Split(true,
                      {ListLeaf("list"), TerminalLeaf("terminal", "list"),
                       TerminalLeaf("terminal-2", "list"), ListLeaf("list2")},
                      {LayoutSize::Rest(), LayoutSize::Px(300),
                       LayoutSize::Px(200), LayoutSize::Px(240)})};
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
    // layout once, and what it keeps is the layout the window shows. Every
    // `store.patch` is announced with the document it made, also when it
    // changed nothing (StoreService), so a second write of the same state
    // would be a second event with that document, and the count of one is
    // the count of the requests.
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

    // The window with `layout` of the four panels of the cases below; the
    // terminals are not settled (a narrow one is narrower than the greeting).
    void BootWith(const Layout& layout) {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Shell_->SetLayout(layout);
        WaitForLayout({{"list", "terminal"}}, false);
        for (const char* instance : {"list2", "terminal-2"}) {
            QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance(instance) != nullptr,
                                     TIMEOUT_MS);
        }
    }

    double Wide(const QString& instance) {
        return SlotByInstance(instance)->width();
    }

    // The split that holds the slot.
    QQuickItem* SplitOf(const QString& instance) {
        for (QQuickItem* item = SlotByInstance(instance)->parentItem();
             item != nullptr; item = item->parentItem()) {
            if (item->property("handleSize").isValid()) {
                return item;
            }
        }
        return nullptr;
    }

    // The two terminals fill the room the lists left, with one divider
    // between them and none beside a list that is collapsed.
    bool TerminalsFillTheSplit() {
        QQuickItem* split = SplitOf("terminal");
        if (split == nullptr || !Shown("terminal") || !Shown("terminal-2")) {
            return false;
        }
        const QQuickItem* first = SlotByInstance("terminal");
        const QQuickItem* second = SlotByInstance("terminal-2");
        const qreal left = first->mapToItem(split, QPointF(0, 0)).x();
        const qreal right =
            second->mapToItem(split, QPointF(second->width(), 0)).x();
        const qreal between =
            second->mapToItem(split, QPointF(0, 0)).x() -
            first->mapToItem(split, QPointF(first->width(), 0)).x();
        return qAbs(left) < 0.5 && qAbs(right - split->width()) < 0.5 &&
               qAbs(between - split->property("handleSize").toReal()) < 0.5;
    }

    // A drag with the mouse from `from` by `dx`, in steps, after a frame that
    // has laid out what the window has now: the dividers are where the slots
    // are, and a press on one finds it.
    void Drag(QPoint from, int dx) {
        QSignalSpy frame(Window(), &QQuickWindow::frameSwapped);
        Window()->update();
        QVERIFY(frame.wait(TIMEOUT_MS) || !frame.isEmpty());
        QTest::mousePress(Window(), Qt::LeftButton, Qt::NoModifier, from);
        for (int step = 1; step <= 8; ++step) {
            QTest::mouseMove(Window(), from + QPoint(dx * step / 8, 0));
        }
        QTest::mouseRelease(Window(), Qt::LeftButton, Qt::NoModifier,
                            from + QPoint(dx, 0));
    }

    // The point on the divider to the right of the slot.
    QPoint DividerRightOf(const QString& instance) {
        QQuickItem* slot = SlotByInstance(instance);
        return slot->mapToScene(QPointF(slot->width(), 60)).toPoint();
    }

    std::vector<LayoutSize> RootSizes() {
        return Shell_->CurrentLayout().Root.Sizes;
    }

    // The document the window shows is the one roostd keeps.
    void WaitForStoredLayout() {
        QTRY_COMPARE_WITH_TIMEOUT(Json(StoredLayout()),
                                  Json(LayoutToJson(Shell_->CurrentLayout())),
                                  TIMEOUT_MS);
    }

    // A layout document written before roostd starts.
    void SeedLayout(const Layout& layout) {
        QVERIFY(QDir().mkpath(Dir_->filePath("config")));
        QSaveFile file(Dir_->filePath("config/layout.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(Json(LayoutToJson(layout))) > 0);
        QVERIFY(file.commit());
    }

    // The tab buttons that are shown, left to right.
    QList<QQuickItem*> ShownTabButtons() {
        QList<QQuickItem*> buttons;
        CollectByClass(Window()->contentItem(), "TabButton", buttons);
        buttons.removeIf([](const QQuickItem* tab) {
            return !tab->isVisible() || tab->width() < 1;
        });
        std::ranges::sort(buttons, {}, [](const QQuickItem* tab) {
            return tab->mapToScene(QPointF(0, 0)).x();
        });
        return buttons;
    }

    static void CollectByClass(QQuickItem* item, const char* name,
                               QList<QQuickItem*>& out) {
        for (QQuickItem* child : item->childItems()) {
            const QString cls = child->metaObject()->className();
            const auto length = static_cast<qsizetype>(qstrlen(name));
            if (cls.startsWith(name) &&
                (cls.size() == length || cls.at(length) == '_')) {
                out.append(child);
            }
            CollectByClass(child, name, out);
        }
    }

    // Saves a picture of the window as <case>-<step>.png when asked to.
    void Snapshot(const QString& step) {
        const QString directory = qEnvironmentVariable("ROOST_TEST_SNAPSHOTS");
        if (directory.isEmpty()) {
            return;
        }
        QVERIFY(QDir().mkpath(directory));
        const QString name =
            QStringLiteral("%1-%2.png")
                .arg(QString::fromLatin1(QTest::currentTestFunction()), step);
        // The content item, not the window: grabWindow distorts the terminal
        // in the offscreen platform.
        QSharedPointer<QQuickItemGrabResult> result =
            Window()->contentItem()->grabToImage();
        QVERIFY(!result.isNull());
        QSignalSpy ready(result.data(), &QQuickItemGrabResult::ready);
        QVERIFY(ready.wait(TIMEOUT_MS) || !ready.isEmpty());
        QVERIFY(result->saveToFile(QDir(directory).filePath(name)));
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
        // The write of the layout set above has landed, so that the check at
        // the end compares with it and not with a write still on its way.
        QTRY_COMPARE_WITH_TIMEOUT(StoredLayout(), LayoutToJson(lists),
                                  TIMEOUT_MS);
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

    void ADragOfTheShownSiblingsKeepsTheSizesOfTheHiddenLists() {
        BootWith(TerminalsBetweenLists());
        if (QTest::currentTestFailed()) {
            return;
        }

        QTRY_COMPARE_WITH_TIMEOUT(Wide("list"), 320.0, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Wide("list2"), 240.0, TIMEOUT_MS);
        const std::vector<LayoutSize> sizes = RootSizes();

        Toggle("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(!SlotByInstance("list")->isVisible() &&
                                     !SlotByInstance("list2")->isVisible(),
                                 TIMEOUT_MS);
        // No room for the lists, and no divider beside them.
        QTRY_VERIFY_WITH_TIMEOUT(TerminalsFillTheSplit(), TIMEOUT_MS);
        Snapshot("1-hidden");
        const double beforeDrag = Wide("terminal");
        // Narrower, so that the room is enough for the lists to come back
        // beside both terminals (the window margins and the gaps between the
        // cards take room too).
        Drag(DividerRightOf("terminal"), -250);
        QTRY_VERIFY_WITH_TIMEOUT(Wide("terminal") < beforeDrag - 200,
                                 TIMEOUT_MS);
        const int dragged = qRound(Wide("terminal"));
        // The terminals are as the user left them, the lists as they were.
        const std::vector<LayoutSize> moved = {
            sizes[0], LayoutSize::Px(dragged), LayoutSize::Rest(), sizes[3]};
        QTRY_COMPARE_WITH_TIMEOUT(RootSizes(), moved, TIMEOUT_MS);
        WaitForStoredLayout();
        QVERIFY(Shell_->CurrentLayout().ListsHidden);
        QCOMPARE(StoredLayout().value("root").toObject().value("sizes"),
                 LayoutToJson(Shell_->CurrentLayout())
                     .value("root")
                     .toObject()
                     .value("sizes"));
        QVERIFY(TerminalsFillTheSplit());

        Toggle("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list") && Shown("list2"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Wide("list"), 320.0, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Wide("list2"), 240.0, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(Wide("terminal") - dragged) <= 1,
                                 TIMEOUT_MS);
        QVERIFY(Wide("terminal-2") >= 40);
        Snapshot("2-shown");
        QCOMPARE(RootSizes(), moved);
        WaitForStoredLayout();
        QVERIFY(!StoredLayout().contains("listsHidden"));
    }

    void ADragWhenNoShownChildFillsKeepsTheSizeOfTheOneThatTakesTheRoom() {
        BootWith(FillListFirst());
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Wide("terminal"), 300.0, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Wide("terminal-2"), 200.0, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Wide("list2"), 240.0, TIMEOUT_MS);
        const std::vector<LayoutSize> sizes = RootSizes();

        // The last terminal is the one that is shown and takes the room.
        Toggle("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(!SlotByInstance("list")->isVisible() &&
                                     !SlotByInstance("list2")->isVisible(),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(TerminalsFillTheSplit(), TIMEOUT_MS);
        QCOMPARE(Wide("terminal"), 300.0);
        Drag(DividerRightOf("terminal"), -60);
        QTRY_VERIFY2_WITH_TIMEOUT(
            Wide("terminal") < 260,
            qPrintable(QStringLiteral("t %1 t2 %2 press at %3")
                           .arg(Wide("terminal"))
                           .arg(Wide("terminal-2"))
                           .arg(DividerRightOf("terminal").x())),
            TIMEOUT_MS);
        const int dragged = qRound(Wide("terminal"));
        // The terminal that took the room stays as stored.
        const std::vector<LayoutSize> moved = {
            LayoutSize::Rest(), LayoutSize::Px(dragged), sizes[2], sizes[3]};
        QTRY_COMPARE_WITH_TIMEOUT(RootSizes(), moved, TIMEOUT_MS);
        WaitForStoredLayout();

        Toggle("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list") && Shown("list2"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Wide("list2"), 240.0, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Wide("terminal-2"), 200.0, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(Wide("terminal") - dragged) <= 1,
                                 TIMEOUT_MS);
        QVERIFY(Wide("list") > 100);
        QCOMPARE(RootSizes(), moved);
        WaitForStoredLayout();
    }

    void TheLastListGoingWhileHiddenBringsTheStateBackAndANewListIsShown() {
        BootWithTwoLists();
        if (QTest::currentTestFailed()) {
            return;
        }
        Toggle("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(!SlotByInstance("list")->isVisible(),
                                 TIMEOUT_MS);
        // A document with no list left, as another edit of the file or an
        // older roost writes it, still has the state.
        LayoutNode terminal;
        terminal.Instance = "terminal";
        terminal.Panel = "terminal.view";
        Shell_->SetLayout(WithState(Layout{.Root = terminal}, true));
        QVERIFY(!Shell_->CurrentLayout().ListsHidden);
        QTRY_VERIFY_WITH_TIMEOUT(!ListsHiddenOfWindow(), TIMEOUT_MS);
        WaitForStoredLayout();
        QVERIFY(!StoredLayout().contains("listsHidden"));

        // A list split in later is not collapsed.
        Focus("terminal");
        Api_->Commands().Run("layout.splitDown",
                             QJsonObject{{"panel", "sessions.list"}});
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("sessions.list-2") != nullptr,
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Shown("sessions.list-2"), TIMEOUT_MS);
        QVERIFY(!Shell_->CurrentLayout().ListsHidden);
        QVERIFY(!ListsHiddenOfWindow());
        WaitForStoredLayout();
        QVERIFY(!StoredLayout().contains("listsHidden"));
    }

    void AStoredStateOfAWindowWithOnlyListsShowsThemAndGoesWithTheNextEdit() {
        // No terminal in the window: the document has only lists.
        WithoutTerminal_ = true;
        const Layout lists = {
            .Root = Split(true, {ListLeaf("list"), ListLeaf("list2")},
                          {LayoutSize::Px(320), LayoutSize::Rest()})};
        SeedLayout(WithState(lists, true));
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list") && Shown("list2"), TIMEOUT_MS);
        QCOMPARE(Shell_->CurrentLayout(), lists);
        QVERIFY(!ListsHiddenOfWindow());
        QVERIFY(Problems().isEmpty());
        // Reading does not write: the state is cleared in the window, and
        // goes from the document with the next edit.
        Barrier();
        QCOMPARE(StoredLayout().value("listsHidden"), QJsonValue(true));

        Focus("list");
        Api_->Commands().Run("layout.splitDown");
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("list-2") != nullptr,
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list-2"), TIMEOUT_MS);
        WaitForStoredLayout();
        QVERIFY(!StoredLayout().contains("listsHidden"));
        QVERIFY(Shown("list") && Shown("list2"));
    }

    void AHiddenTabHasNoGapInTheBar() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        const Layout stack = {
            .Root =
                Tabs({TerminalLeaf("terminal", "list"), ListLeaf("list"),
                      TerminalLeaf("terminal-2", "list"), ListLeaf("list2")},
                     0)};
        Shell_->SetLayout(stack);
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("list") != nullptr &&
                                     PanelByInstance("list2") != nullptr &&
                                     PanelByInstance("terminal") != nullptr &&
                                     PanelByInstance("terminal-2") != nullptr,
                                 TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(static_cast<int>(ShownTabButtons().size()), 4,
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Shown("terminal"), TIMEOUT_MS);
        // The space between two buttons of the bar as it is.
        const auto gapAfter = [](const QList<QQuickItem*>& buttons, int i) {
            return buttons.at(i + 1)->mapToScene(QPointF(0, 0)).x() -
                   buttons.at(i)
                       ->mapToScene(QPointF(buttons.at(i)->width(), 0))
                       .x();
        };
        const double spacing = gapAfter(ShownTabButtons(), 0);
        Snapshot("1-shown");

        Toggle("terminal");
        QTRY_COMPARE_WITH_TIMEOUT(static_cast<int>(ShownTabButtons().size()), 2,
                                  TIMEOUT_MS);
        const QList<QQuickItem*> buttons = ShownTabButtons();
        QCOMPARE(buttons.at(0)->property("text").toString(),
                 QString("terminal"));
        QCOMPARE(buttons.at(1)->property("text").toString(),
                 QString("terminal-2"));
        // The same space as between any two tabs, not one more for each tab
        // that is not shown.
        QVERIFY2(qAbs(gapAfter(buttons, 0) - spacing) < 0.5,
                 qPrintable(QStringLiteral("gap %1, spacing %2")
                                .arg(gapAfter(buttons, 0))
                                .arg(spacing)));
        Snapshot("2-hidden");
    }
};

QTEST_MAIN(LayoutListsTest)
#include "layout_lists_test.moc"
