// Swap a panel with another one (`layout.swap`) and move a tab
// (`layout.tabLeft`, `layout.tabRight`). The first half is the pure parts on
// trees (no window): the shape, the sizes, the links and instances that stay,
// the refusals and the canonical tree. The second half is the real window
// against a real roostd: the menu items and the commands, one write of the
// layout per edit, the layout that comes back after a restart, the refusal in
// the problems strip, and the undo of each edit.

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlExtensionPlugin>
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

LayoutNode Leaf(const QString& instance, const QString& panel,
                const QMap<QString, LayoutLink>& links = {}) {
    LayoutNode node;
    node.Instance = instance;
    node.Panel = panel;
    node.Links = links;
    return node;
}

LayoutLink From(const QString& from) {
    return {.From = from, .Output = "selected", .Problem = {}};
}

LayoutNode Term(const QString& instance) {
    return Leaf(instance, "terminal.view", {{"session", From("list")}});
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

// The list, 500 wide, left of two terminals one under the other.
Layout ListAndTwoTerminals() {
    return {.Root = Split(true,
                          {Leaf("list", "sessions.list"),
                           Split(false, {Term("terminal"), Term("t2")},
                                 {LayoutSize::Px(200), LayoutSize::Rest()})},
                          {LayoutSize::Px(500), LayoutSize::Rest()})};
}

// The list and a stack of three terminals.
Layout ListAndTabs(int active) {
    return {.Root = Split(
                true,
                {Leaf("list", "sessions.list"),
                 Tabs({Term("terminal"), Term("t2"), Term("t3")}, active)},
                {LayoutSize::Px(320), LayoutSize::Rest()})};
}

QStringList Instances(const LayoutNode& node) {
    if (node.Kind == ELayoutNodeKind::Leaf) {
        return {node.Instance};
    }
    QStringList all;
    for (const LayoutNode& child : node.Children) {
        all.append(Instances(child));
    }
    return all;
}

// The leaf that is looked at when the node is: the first of a split, the shown
// tab of a stack (with the lists of `hidden` types out of the count).
QString ShownLeafOf(const LayoutNode& node, const QSet<QString>& hidden) {
    if (node.Kind == ELayoutNodeKind::Leaf) {
        return node.Instance;
    }
    const size_t at = node.Kind == ELayoutNodeKind::Tabs
                          ? static_cast<size_t>(ShownTab(node, hidden))
                          : 0;
    return ShownLeafOf(node.Children[at], hidden);
}

// The tree is canonical: no split or stack with one child, a split has a child
// that takes the rest, sizes one per child.
bool IsCanonical(const LayoutNode& node) {
    if (node.Kind == ELayoutNodeKind::Leaf) {
        return true;
    }
    if (node.Children.size() < 2) {
        return false;
    }
    if (node.Kind == ELayoutNodeKind::Tabs) {
        if (node.Active < 0 ||
            node.Active >= static_cast<int>(node.Children.size())) {
            return false;
        }
    } else if (node.Sizes.size() != node.Children.size() ||
               std::ranges::none_of(node.Sizes, [](const LayoutSize& size) {
                   return size.Fill;
               })) {
        return false;
    }
    return std::ranges::all_of(node.Children, IsCanonical);
}

void ExpectRefused(const Layout& before, const LayoutEdit& edit,
                   const QString& reason) {
    QVERIFY2(edit.Refused.contains(reason), qPrintable(edit.Refused));
    QVERIFY(!edit.Changed);
    QCOMPARE(edit.Tree, before);
}

}  // namespace

class LayoutSwapTabsTest : public LayoutWindow {
    Q_OBJECT

private:
    std::optional<ScopedEnv> Home_;
    std::optional<ScopedEnv> Socket_;
    std::optional<ScopedEnv> Config_;

    // The writes of the layout document the window has seen since the spy was
    // made: each is one write of roostd.
    static int Writes(const QSignalSpy& spy) {
        int count = 0;
        for (const auto& call : spy) {
            if (call.at(0).toString() == QLatin1String("layout")) {
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

    // Runs the command with the keyboard in `instance`; roostd writes the
    // layout once, and what it keeps is what the window shows.
    void RunWritesOnce(const QString& instance, const QString& command,
                       const QJsonValue& args = QJsonValue::Undefined) {
        Focus(instance);
        QVERIFY(Api_->Commands().HasLocal(command));
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Api_->Commands().Run(command, args);
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredLayout(), LayoutToJson(Shell_->CurrentLayout()), TIMEOUT_MS);
        QCOMPARE(Writes(spy), 1);
    }

    // The command is refused: the strip says why, and nothing is written. A
    // write that was not going to come is not waited for: the barrier is an
    // edit that works, made after the refusal and written by the same daemon
    // after any write the refusal would have made; it is the only write the
    // spy sees. The edit is undone, so the layout is what it was.
    void RunRefused(const QString& instance, const QString& command,
                    const QJsonValue& args, const QString& reason) {
        Focus(instance);
        const Layout before = Shell_->CurrentLayout();
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Api_->Commands().Run(command, args);
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem(reason), TIMEOUT_MS);
        QCOMPARE(Shell_->CurrentLayout(), before);
        const QStringList all = Instances(before.Root);
        const QString with = all.first() != instance ? all.first() : all.last();
        Api_->Commands().Run("layout.swap", QJsonObject{{"with", with}});
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Shell_->CurrentLayout() != before, TIMEOUT_MS);
        QCOMPARE(Writes(spy), 1);
        RunWritesOnce(instance, "layout.undo");
        QCOMPARE(Shell_->CurrentLayout(), before);
    }

    bool HasProblem(const QString& part) {
        for (const QString& problem : Problems()) {
            if (problem.contains(part)) {
                return true;
            }
        }
        return false;
    }

    static QPoint CenterOf(QQuickItem* item) {
        return item->mapToScene(QPointF(item->width() / 2, item->height() / 2))
            .toPoint();
    }

    QQuickItem* Item(const QString& name) {
        return Root()->findChild<QQuickItem*>(name);
    }

    // The menu of the panel, opened with the right button on it.
    void OpenMenuOn(const QString& instance) {
        // The menu that was open is closed (a closing one takes no click).
        if (QObject* open = Root()->findChild<QObject*>("panelMenu")) {
            QTRY_VERIFY_WITH_TIMEOUT(!open->property("visible").toBool(),
                                     TIMEOUT_MS);
        }
        // The slot is laid out, and the keyboard has gone where the last edit
        // put it: a click earlier lands on a slot being replaced.
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance(instance) != nullptr &&
                                     SlotByInstance(instance)->isVisible() &&
                                     SlotByInstance(instance)->width() > 100,
                                 TIMEOUT_MS);
        QQuickItem* slot = SlotByInstance(instance);
        QVERIFY(slot != nullptr);
        QTest::mouseMove(Window(), slot->mapToScene(QPointF(40, 40)).toPoint());
        QTest::mouseClick(Window(), Qt::RightButton, Qt::NoModifier,
                          slot->mapToScene(QPointF(40, 40)).toPoint());
        QObject* popup = Root()->findChild<QObject*>("panelMenu");
        QVERIFY(popup != nullptr);
        QTRY_VERIFY2_WITH_TIMEOUT(popup->property("visible").toBool() &&
                                      popup->property("instance") == instance,
                                  qPrintable("menu of " + instance),
                                  TIMEOUT_MS);
    }

    // The row of the menu whose submenu is `submenu`.
    QQuickItem* RowOfSubmenu(QObject* submenu) {
        QObject* menu = Root()->findChild<QObject*>("panelMenu");
        for (int i = 0; i < 64; ++i) {
            QQuickItem* candidate = nullptr;
            if (!QMetaObject::invokeMethod(menu, "itemAt",
                                           Q_RETURN_ARG(QQuickItem*, candidate),
                                           Q_ARG(int, i)) ||
                candidate == nullptr) {
                break;
            }
            if (candidate->property("subMenu").value<QObject*>() == submenu) {
                return candidate;
            }
        }
        return nullptr;
    }

    // Opens "Swap with…" of the menu that is open and clicks the panel's row.
    void ClickSwapWith(const QString& with) {
        QObject* submenu = Root()->findChild<QObject*>("menuSwapWith");
        QVERIFY(submenu != nullptr);
        QQuickItem* row = RowOfSubmenu(submenu);
        QVERIFY(row != nullptr);
        QTest::mouseClick(Window(), Qt::LeftButton, Qt::NoModifier,
                          CenterOf(row));
        QTRY_VERIFY_WITH_TIMEOUT(submenu->property("visible").toBool(),
                                 TIMEOUT_MS);
        QQuickItem* entry = Item("menuSwapWith_" + with);
        QVERIFY(entry != nullptr);
        QVERIFY(entry->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(entry->isVisible() && entry->width() > 0,
                                 TIMEOUT_MS);
        QTest::mouseClick(Window(), Qt::LeftButton, Qt::NoModifier,
                          CenterOf(entry));
    }

    // The names of the tab buttons of the window, from the left.
    QStringList TabOrder() {
        QList<QQuickItem*> texts;
        const auto walk = [&texts](auto&& self, QQuickItem* item) -> void {
            if (item->objectName() == QLatin1String("tabText") &&
                item->isVisible() && item->width() > 0) {
                texts.append(item);
            }
            for (QQuickItem* child : item->childItems()) {
                self(self, child);
            }
        };
        walk(walk, Window()->contentItem());
        std::ranges::sort(texts, [](QQuickItem* a, QQuickItem* b) {
            return a->mapToScene(QPointF(0, 0)).x() <
                   b->mapToScene(QPointF(0, 0)).x();
        });
        QStringList names;
        for (QQuickItem* text : texts) {
            names.append(text->property("text").toString());
        }
        return names;
    }

    // The layout is set and the window shows its panels (SetLayout is no edit).
    void StartWith(const Layout& layout) {
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Shell_->SetLayout(layout);
        QTRY_COMPARE_WITH_TIMEOUT(Shell_->CurrentLayout(), layout, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Writes(spy) >= 1, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(StoredLayout(), LayoutToJson(layout),
                                  TIMEOUT_MS);
    }

    void BootWith(const Layout& layout) {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        StartWith(layout);
    }

    // The window is closed and opened again on the same daemon and config.
    void Restart() {
        CloseWindow();
        StopDaemon();
        QVERIFY(StartDaemon());
        OpenWindow();
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

    // ---- swap: the pure parts ---------------------------------------------

    // The list stays 320 wide: it takes its size to the other side.
    void SwapOfTwoSiblingsTakesTheSizesAlong() {
        const Layout layout = {
            .Root =
                Split(true, {Leaf("list", "sessions.list"), Term("terminal")},
                      {LayoutSize::Px(320), LayoutSize::Rest()})};
        const LayoutEdit edit = SwapPanels(layout, "list", "terminal", Known());
        QVERIFY(edit.Refused.isEmpty());
        QVERIFY(edit.Changed);
        QCOMPARE(Instances(edit.Tree.Root), (QStringList{"terminal", "list"}));
        QCOMPARE(
            edit.Tree.Root.Sizes,
            (std::vector<LayoutSize>{LayoutSize::Rest(), LayoutSize::Px(320)}));
        QCOMPARE(edit.Focus, QString("list"));
        QVERIFY(IsCanonical(edit.Tree.Root));
    }

    // Different parents: the places keep their sizes, and nothing else of the
    // tree moves; the leaves keep their instance, type and links.
    void SwapAcrossSplitsKeepsTheSizesOfThePlacesAndTheLeaves() {
        const Layout layout = ListAndTwoTerminals();
        const LayoutEdit edit = SwapPanels(layout, "list", "t2", Known());
        QVERIFY(edit.Refused.isEmpty());
        QVERIFY(edit.Changed);
        QCOMPARE(Instances(edit.Tree.Root),
                 (QStringList{"t2", "terminal", "list"}));
        QCOMPARE(edit.Tree.Root.Sizes, layout.Root.Sizes);
        QCOMPARE(edit.Tree.Root.Children[1].Sizes,
                 layout.Root.Children[1].Sizes);
        QCOMPARE(edit.Tree.Root.Children[1].Horizontal, false);
        QCOMPARE(CountLeaves(edit.Tree.Root), 3);
        // The leaves are the same ones, whole.
        QCOMPARE(*FindLeaf(edit.Tree.Root, "t2"), *FindLeaf(layout.Root, "t2"));
        QCOMPARE(*FindLeaf(edit.Tree.Root, "list"),
                 *FindLeaf(layout.Root, "list"));
        QCOMPARE(*FindLeaf(edit.Tree.Root, "terminal"),
                 *FindLeaf(layout.Root, "terminal"));
        QCOMPARE(FindLeaf(edit.Tree.Root, "t2")->Links.value("session"),
                 From("list"));
        QVERIFY(IsCanonical(edit.Tree.Root));
    }

    void SwapTwiceGivesTheTreeBack() {
        for (const Layout& layout : {ListAndTwoTerminals(), ListAndTabs(1)}) {
            const LayoutEdit once = SwapPanels(layout, "list", "t2", Known());
            QVERIFY(once.Changed);
            const LayoutEdit twice =
                SwapPanels(once.Tree, "list", "t2", Known());
            QCOMPARE(twice.Tree, layout);
        }
    }

    // A leaf that was shown is the shown tab of the stack it goes to.
    void SwapIntoAStackShowsTheLeavesThatWereShown() {
        const LayoutEdit edit =
            SwapPanels(ListAndTabs(1), "list", "t2", Known());
        QVERIFY(edit.Refused.isEmpty());
        const LayoutNode& stack = edit.Tree.Root.Children[1];
        QCOMPARE(Instances(stack), (QStringList{"terminal", "list", "t3"}));
        QCOMPARE(stack.Active, 1);
        QCOMPARE(edit.Tree.Root.Children[0].Instance, QString("t2"));
        QCOMPARE(edit.Tree.Root.Sizes, ListAndTabs(1).Root.Sizes);
        QVERIFY(IsCanonical(edit.Tree.Root));
        // A hidden tab and the list: the list was shown, so it is the shown
        // tab of the stack it goes to.
        const LayoutEdit hidden =
            SwapPanels(ListAndTabs(0), "t3", "list", Known());
        QCOMPARE(Instances(hidden.Tree.Root.Children[1]),
                 (QStringList{"terminal", "t2", "list"}));
        QCOMPARE(hidden.Tree.Root.Children[1].Active, 2);
    }

    // Both in one stack: the shown tab is the same panel as before.
    void SwapInOneStackKeepsWhichPanelIsShown() {
        const Layout layout = {.Root =
                                   Tabs({Term("a"), Term("b"), Term("c")}, 0)};
        const LayoutEdit edit = SwapPanels(layout, "a", "c", Known());
        QCOMPARE(Instances(edit.Tree.Root), (QStringList{"c", "b", "a"}));
        QCOMPARE(edit.Tree.Root.Active, 2);
        // Neither of the two is shown: the shown one stays where it is.
        const LayoutEdit others = SwapPanels(layout, "b", "c", Known());
        QCOMPARE(Instances(others.Tree.Root), (QStringList{"a", "c", "b"}));
        QCOMPARE(others.Tree.Root.Active, 0);
        QVERIFY(IsCanonical(edit.Tree.Root));
    }

    // The shown leaf is looked at through every stack above it: x goes into
    // the hidden branch of the stack, so the branch is shown, and y takes the
    // place of x.
    void SwapIntoAHiddenBranchOfANestedStackKeepsTheLeafShown() {
        const Layout layout = {
            .Root =
                Split(true,
                      {Term("x"),
                       Tabs({Split(false, {Term("y"), Term("z")},
                                   {LayoutSize::Px(100), LayoutSize::Rest()}),
                             Term("w")},
                            1)},
                      {LayoutSize::Px(200), LayoutSize::Rest()})};
        for (const auto& [instance, with] :
             {std::pair<QString, QString>{"x", "y"}, {"y", "x"}}) {
            const LayoutEdit edit = SwapPanels(layout, instance, with, Known());
            QVERIFY(edit.Refused.isEmpty());
            QCOMPARE(Instances(edit.Tree.Root),
                     (QStringList{"y", "x", "z", "w"}));
            const LayoutNode& stack = edit.Tree.Root.Children[1];
            QCOMPARE(stack.Active, 0);
            QCOMPARE(ShownLeafOf(edit.Tree.Root, {}), QString("y"));
            QCOMPARE(ShownLeafOf(stack, {}), QString("x"));
            QVERIFY(IsCanonical(edit.Tree.Root));
        }
    }

    // With the lists hidden the terminal of [list, terminal] is the shown tab
    // although the stack says tab 0: it stays shown in the other stack.
    void SwapOfTheTabThatIsShownBecauseTheListsAreHidden() {
        Layout layout = {
            .Root = Split(
                true,
                {Tabs({Leaf("list", "sessions.list"), Term("terminal")}, 0),
                 Tabs({Term("u"), Term("v")}, 0)},
                {LayoutSize::Px(300), LayoutSize::Rest()})};
        layout.ListsHidden = true;
        QCOMPARE(
            ShownLeafOf(layout.Root.Children[0], HiddenTypes(layout, Known())),
            QString("terminal"));
        const LayoutEdit edit = SwapPanels(layout, "terminal", "v", Known());
        QVERIFY(edit.Refused.isEmpty());
        QCOMPARE(Instances(edit.Tree.Root),
                 (QStringList{"list", "v", "u", "terminal"}));
        QCOMPARE(edit.Tree.Root.Children[1].Active, 1);
        QCOMPARE(ShownLeafOf(edit.Tree.Root.Children[1],
                             HiddenTypes(edit.Tree, Known())),
                 QString("terminal"));
    }

    // Two different stacks: both leaves were shown and both still are.
    void SwapBetweenTwoStacksKeepsBothShown() {
        const Layout layout = {
            .Root = Split(true,
                          {Tabs({Term("a"), Term("b")}, 0),
                           Tabs({Term("c"), Term("d")}, 1)},
                          {LayoutSize::Px(300), LayoutSize::Rest()})};
        const LayoutEdit both = SwapPanels(layout, "a", "d", Known());
        QCOMPARE(Instances(both.Tree.Root), (QStringList{"d", "b", "c", "a"}));
        QCOMPARE(both.Tree.Root.Children[0].Active, 0);
        QCOMPARE(both.Tree.Root.Children[1].Active, 1);
        // Only the leaf that was shown has to stay shown.
        const LayoutEdit one = SwapPanels(layout, "b", "d", Known());
        QCOMPARE(Instances(one.Tree.Root), (QStringList{"a", "d", "c", "b"}));
        QCOMPARE(one.Tree.Root.Children[0].Active, 1);
    }

    void SwapIsRefusedForNoPanelItselfAndNoChoice() {
        const Layout layout = ListAndTwoTerminals();
        // The two texts differ: the one that is not there is named.
        const LayoutEdit noInstance =
            SwapPanels(layout, "nope", "list", Known());
        ExpectRefused(layout, noInstance, "there is no panel nope");
        QCOMPARE(noInstance.Refused, QString("there is no panel nope to swap"));
        const LayoutEdit noWith = SwapPanels(layout, "list", "nope", Known());
        ExpectRefused(layout, noWith, "there is no panel nope");
        QCOMPARE(noWith.Refused,
                 QString("there is no panel nope to swap with"));
        ExpectRefused(layout, SwapPanels(layout, "list", "list", Known()),
                      "list cannot be swapped with itself");
        const LayoutEdit noChoice = SwapPanels(layout, "list", "", Known());
        ExpectRefused(layout, noChoice, "choose the panel to swap list with");
        QCOMPARE(noChoice.Refused,
                 QString("choose the panel to swap list with: use \"Swap "
                         "with…\" in the panel menu"));
        const Layout alone = {.Root = Leaf("list", "sessions.list")};
        ExpectRefused(alone, SwapPanels(alone, "list", "terminal", Known()),
                      "there is no panel terminal");
    }

    // ---- tabs: the pure parts ---------------------------------------------

    void TabMovesOnePlaceAndTheShownTabIsTheSamePanel() {
        // The shown tab moves with its panel.
        const LayoutEdit right = MoveTab(ListAndTabs(1), "t2", true, Known());
        QVERIFY(right.Refused.isEmpty());
        QVERIFY(right.Changed);
        QCOMPARE(Instances(right.Tree.Root.Children[1]),
                 (QStringList{"terminal", "t3", "t2"}));
        QCOMPARE(right.Tree.Root.Children[1].Active, 2);
        QCOMPARE(right.Focus, QString("t2"));
        // The rest of the tree is as it was.
        QCOMPARE(right.Tree.Root.Children[0], ListAndTabs(1).Root.Children[0]);
        QCOMPARE(right.Tree.Root.Sizes, ListAndTabs(1).Root.Sizes);
        QVERIFY(IsCanonical(right.Tree.Root));

        const LayoutEdit left = MoveTab(ListAndTabs(1), "t2", false, Known());
        QCOMPARE(Instances(left.Tree.Root.Children[1]),
                 (QStringList{"t2", "terminal", "t3"}));
        QCOMPARE(left.Tree.Root.Children[1].Active, 0);

        // A hidden tab that passes the shown one: the shown one steps aside.
        const LayoutEdit passes = MoveTab(ListAndTabs(1), "t3", false, Known());
        QCOMPARE(Instances(passes.Tree.Root.Children[1]),
                 (QStringList{"terminal", "t3", "t2"}));
        QCOMPARE(passes.Tree.Root.Children[1].Active, 2);
        // A hidden tab elsewhere: the shown one stays.
        const LayoutEdit apart = MoveTab(ListAndTabs(0), "t3", false, Known());
        QCOMPARE(Instances(apart.Tree.Root.Children[1]),
                 (QStringList{"terminal", "t3", "t2"}));
        QCOMPARE(apart.Tree.Root.Children[1].Active, 0);
        // Right then left is the tree as it was.
        QCOMPARE(MoveTab(right.Tree, "t2", false, Known()).Tree,
                 ListAndTabs(1));
    }

    // The tab of a hidden list has no button: the move steps over it, and the
    // first tab with a button is the first tab.
    void TabMoveStepsOverTheTabsOfHiddenLists() {
        Layout layout = {
            .Root =
                Tabs({Term("a"), Leaf("list", "sessions.list"), Term("b")}, 2)};
        layout.ListsHidden = true;
        const LayoutEdit left = MoveTab(layout, "b", false, Known());
        QVERIFY(left.Refused.isEmpty());
        QCOMPARE(Instances(left.Tree.Root), (QStringList{"b", "list", "a"}));
        QCOMPARE(left.Tree.Root.Active, 0);
        ExpectRefused(layout, MoveTab(layout, "a", false, Known()),
                      "a is already the first tab");
        ExpectRefused(layout, MoveTab(layout, "b", true, Known()),
                      "b is already the last tab");
        // The lists shown again: the list is a tab like any other.
        layout.ListsHidden = false;
        QCOMPARE(Instances(MoveTab(layout, "b", false, Known()).Tree.Root),
                 (QStringList{"a", "b", "list"}));
    }

    void TabMoveKeepsLinksAndInstances() {
        const Layout layout = ListAndTabs(0);
        const LayoutEdit edit = MoveTab(layout, "terminal", true, Known());
        QVERIFY(edit.Changed);
        QCOMPARE(*FindLeaf(edit.Tree.Root, "terminal"),
                 *FindLeaf(layout.Root, "terminal"));
        QCOMPARE(FindLeaf(edit.Tree.Root, "terminal")->Links.value("session"),
                 From("list"));
        QCOMPARE(CountLeaves(edit.Tree.Root), 4);
    }

    void TabMoveIsRefusedAtTheEndsAndOutsideAStack() {
        const Layout layout = ListAndTabs(0);
        ExpectRefused(layout, MoveTab(layout, "terminal", false, Known()),
                      "terminal is already the first tab");
        ExpectRefused(layout, MoveTab(layout, "t3", true, Known()),
                      "t3 is already the last tab");
        ExpectRefused(layout, MoveTab(layout, "list", true, Known()),
                      "list is not in tabs");
        ExpectRefused(layout, MoveTab(layout, "nope", true, Known()),
                      "there is no panel nope");
        const Layout alone = {.Root = Leaf("list", "sessions.list")};
        ExpectRefused(alone, MoveTab(alone, "list", false, Known()),
                      "list is not in tabs");
    }

    // ---- swap: the window --------------------------------------------------

    void TheSwapCommandExchangesTheListAndTheTerminal() {
        BootWith(ListAndTwoTerminals());
        if (QTest::currentTestFailed()) {
            return;
        }
        const Layout before = Shell_->CurrentLayout();
        RunWritesOnce("list", "layout.swap", QJsonObject{{"with", "t2"}});
        const Layout expected = SwapPanels(before, "list", "t2", Known()).Tree;
        QCOMPARE(Shell_->CurrentLayout(), expected);
        QCOMPARE(StoredLayout(), LayoutToJson(expected));
        // The panels are on their new places in the window, the sizes of the
        // places the same, and the keyboard is where it was.
        QTRY_VERIFY_WITH_TIMEOUT(
            SlotByInstance("t2") != nullptr &&
                SlotByInstance("list") != nullptr &&
                SlotByInstance("t2")->mapToScene(QPointF(0, 0)).x() <
                    SlotByInstance("terminal")->mapToScene(QPointF(0, 0)).x() &&
                SlotByInstance("list")->mapToScene(QPointF(0, 0)).y() >
                    SlotByInstance("terminal")->mapToScene(QPointF(0, 0)).y(),
            TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(SlotByInstance("t2")->width() - 500) < 8,
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("list")->hasActiveFocus(),
                                 TIMEOUT_MS);
        QVERIFY(Problems().isEmpty());
    }

    // The list keeps its 320 pixels on the other side of the terminal.
    void TheListKeepsItsSizeOnTheOtherSideOfTheTerminal() {
        BootWith({.Root = Split(
                      true, {Leaf("list", "sessions.list"), Term("terminal")},
                      {LayoutSize::Px(320), LayoutSize::Rest()})});
        if (QTest::currentTestFailed()) {
            return;
        }
        RunWritesOnce("list", "layout.swap", QJsonObject{{"with", "terminal"}});
        QTRY_VERIFY_WITH_TIMEOUT(
            SlotByInstance("list") != nullptr &&
                SlotByInstance("terminal") != nullptr &&
                SlotByInstance("list")->mapToScene(QPointF(0, 0)).x() >
                    SlotByInstance("terminal")->mapToScene(QPointF(0, 0)).x() &&
                qAbs(SlotByInstance("list")->width() - 320) < 8,
            TIMEOUT_MS);
        QVERIFY(SlotByInstance("terminal")->width() > 320);
    }

    void TheSwapMenuListsTheOtherPanelsAndSwaps() {
        BootWith(ListAndTwoTerminals());
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenMenuOn("terminal");
        QObject* submenu = Root()->findChild<QObject*>("menuSwapWith");
        QVERIFY(submenu != nullptr);
        QVERIFY(RowOfSubmenu(submenu)->isEnabled());
        // The other panels, not the one the menu is open on.
        QVERIFY(Item("menuSwapWith_list") != nullptr);
        QVERIFY(Item("menuSwapWith_t2") != nullptr);
        QVERIFY(Item("menuSwapWith_terminal") == nullptr);
        QCOMPARE(Item("menuSwapWith_t2")->property("text").toString(),
                 QString("t2 (terminal.view)"));
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        const Layout before = Shell_->CurrentLayout();
        ClickSwapWith("list");
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        QCOMPARE(Shell_->CurrentLayout(),
                 SwapPanels(before, "terminal", "list", Known()).Tree);
        QCOMPARE(StoredLayout(), LayoutToJson(Shell_->CurrentLayout()));
    }

    void TheSwapMenuItemIsOffForTheOnlyPanel() {
        BootWith({.Root = Leaf("list", "sessions.list")});
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenMenuOn("list");
        QObject* submenu = Root()->findChild<QObject*>("menuSwapWith");
        QVERIFY(submenu != nullptr);
        QQuickItem* row = RowOfSubmenu(submenu);
        QVERIFY(row != nullptr);
        QVERIFY(!row->isEnabled());
    }

    void TheSwappedLayoutIsStillThereAfterARestart() {
        BootWith(ListAndTwoTerminals());
        if (QTest::currentTestFailed()) {
            return;
        }
        const Layout before = Shell_->CurrentLayout();
        RunWritesOnce("list", "layout.swap", QJsonObject{{"with", "terminal"}});
        const Layout expected =
            SwapPanels(before, "list", "terminal", Known()).Tree;

        Restart();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Shell_->CurrentLayout(), expected,
                                  TIMEOUT_MS);
        QCOMPARE(StoredLayout(), LayoutToJson(expected));
        QVERIFY(Problems().isEmpty());
    }

    void ASwapThatCannotBeDoneShowsInTheStripAndWritesNothing() {
        BootWith(ListAndTwoTerminals());
        if (QTest::currentTestFailed()) {
            return;
        }
        RunRefused("list", "layout.swap", QJsonObject{{"with", "list"}},
                   "list cannot be swapped with itself");
        RunRefused("list", "layout.swap", QJsonObject{{"with", "nope"}},
                   "there is no panel nope");
        // From the palette there is no argument to take: the panel is asked
        // for.
        RunRefused("list", "layout.swap", QJsonValue::Undefined,
                   "choose the panel to swap list with");
        // The next edit that works clears the reason.
        RunWritesOnce("list", "layout.swap", QJsonObject{{"with", "t2"}});
        QTRY_VERIFY_WITH_TIMEOUT(!HasProblem("choose the panel"), TIMEOUT_MS);
    }

    void TheSwapIsUndone() {
        BootWith(ListAndTwoTerminals());
        if (QTest::currentTestFailed()) {
            return;
        }
        const Layout before = Shell_->CurrentLayout();
        RunWritesOnce("terminal", "layout.swap", QJsonObject{{"with", "t2"}});
        QVERIFY(Shell_->CurrentLayout() != before);
        RunWritesOnce("terminal", "layout.undo");
        QCOMPARE(Shell_->CurrentLayout(), before);
        QCOMPARE(StoredLayout(), LayoutToJson(before));
    }

    // ---- tabs: the window --------------------------------------------------

    void TheTabCommandsChangeTheOrderOfTheTabs() {
        BootWith(ListAndTabs(1));
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(
            TabOrder(), (QStringList{"terminal", "t2", "t3"}), TIMEOUT_MS);
        RunWritesOnce("t2", "layout.tabRight");
        QTRY_COMPARE_WITH_TIMEOUT(
            TabOrder(), (QStringList{"terminal", "t3", "t2"}), TIMEOUT_MS);
        // The shown tab is still t2, and it has the keyboard.
        QCOMPARE(Shell_->CurrentLayout().Root.Children[1].Active, 2);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("t2")->hasActiveFocus(),
                                 TIMEOUT_MS);
        RunWritesOnce("t2", "layout.tabLeft");
        RunWritesOnce("t2", "layout.tabLeft");
        QTRY_COMPARE_WITH_TIMEOUT(
            TabOrder(), (QStringList{"t2", "terminal", "t3"}), TIMEOUT_MS);
        QCOMPARE(StoredLayout(), LayoutToJson(Shell_->CurrentLayout()));
        QVERIFY(Problems().isEmpty());
    }

    void TheTabMenuItemsWorkAndAreOffAtTheEnds() {
        BootWith(ListAndTabs(1));
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenMenuOn("t2");
        QVERIFY(Item("menuTabLeft") != nullptr);
        QVERIFY(Item("menuTabRight") != nullptr);
        QVERIFY(Item("menuTabLeft")->isEnabled());
        QVERIFY(Item("menuTabRight")->isEnabled());
        QObject* move = Root()->findChild<QObject*>("menuMove");
        QVERIFY(move != nullptr);
        QQuickItem* moveRow = RowOfSubmenu(move);
        QVERIFY(moveRow != nullptr);
        QTest::mouseClick(Window(), Qt::LeftButton, Qt::NoModifier,
                          CenterOf(moveRow));
        QTRY_VERIFY_WITH_TIMEOUT(move->property("visible").toBool(),
                                 TIMEOUT_MS);
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        QTest::mouseClick(Window(), Qt::LeftButton, Qt::NoModifier,
                          CenterOf(Item("menuTabLeft")));
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            TabOrder(), (QStringList{"t2", "terminal", "t3"}), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("t2")->hasActiveFocus(),
                                 TIMEOUT_MS);
        // The first tab: left is off, right is on.
        OpenMenuOn("t2");
        QTRY_VERIFY_WITH_TIMEOUT(!Item("menuTabLeft")->isEnabled(), TIMEOUT_MS);
        QVERIFY(Item("menuTabRight")->isEnabled());
        QTest::keyClick(Window(), Qt::Key_Escape);
        // Outside a stack both are off.
        OpenMenuOn("list");
        QTRY_VERIFY_WITH_TIMEOUT(!Item("menuTabLeft")->isEnabled() &&
                                     !Item("menuTabRight")->isEnabled(),
                                 TIMEOUT_MS);
    }

    void TheTabMenuItemIsOffOnTheLastTab() {
        BootWith(ListAndTabs(2));
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenMenuOn("t3");
        QTRY_VERIFY_WITH_TIMEOUT(!Item("menuTabRight")->isEnabled(),
                                 TIMEOUT_MS);
        QVERIFY(Item("menuTabLeft")->isEnabled());
    }

    // The tab of a hidden list has no button: next to it nothing can move.
    void TheTabMenuIgnoresTheTabsOfHiddenLists() {
        Layout layout = ListAndTabs(0);
        layout.Root.Children[1] =
            Tabs({Leaf("list2", "sessions.list"), Term("terminal")}, 0);
        layout.ListsHidden = true;
        BootWith(layout);
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenMenuOn("terminal");
        QVERIFY(Item("menuTabLeft") != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(!Item("menuTabLeft")->isEnabled(), TIMEOUT_MS);
        QVERIFY(!Item("menuTabRight")->isEnabled());
    }

    void ATabThatCannotMoveShowsInTheStripAndWritesNothing() {
        BootWith(ListAndTabs(0));
        if (QTest::currentTestFailed()) {
            return;
        }
        RunRefused("terminal", "layout.tabLeft", QJsonValue::Undefined,
                   "terminal is already the first tab");
        RunRefused("list", "layout.tabRight", QJsonValue::Undefined,
                   "list is not in tabs");
        // The last tab, shown.
        StartWith(ListAndTabs(2));
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("t3") != nullptr &&
                                     SlotByInstance("t3")->isVisible(),
                                 TIMEOUT_MS);
        RunRefused("t3", "layout.tabRight", QJsonValue::Undefined,
                   "t3 is already the last tab");
    }

    void TheTabOrderIsStillThereAfterARestart() {
        BootWith(ListAndTabs(1));
        if (QTest::currentTestFailed()) {
            return;
        }
        RunWritesOnce("t2", "layout.tabRight");
        const Layout expected = Shell_->CurrentLayout();

        Restart();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Shell_->CurrentLayout(), expected,
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            TabOrder(), (QStringList{"terminal", "t3", "t2"}), TIMEOUT_MS);
        QVERIFY(Problems().isEmpty());
    }

    void TheTabMoveIsUndone() {
        BootWith(ListAndTabs(1));
        if (QTest::currentTestFailed()) {
            return;
        }
        const Layout before = Shell_->CurrentLayout();
        RunWritesOnce("t2", "layout.tabRight");
        QVERIFY(Shell_->CurrentLayout() != before);
        RunWritesOnce("t2", "layout.undo");
        QCOMPARE(Shell_->CurrentLayout(), before);
        QCOMPARE(StoredLayout(), LayoutToJson(before));
        QTRY_COMPARE_WITH_TIMEOUT(
            TabOrder(), (QStringList{"terminal", "t2", "t3"}), TIMEOUT_MS);
    }
};

QTEST_MAIN(LayoutSwapTabsTest)
#include "layout_swap_tabs_test.moc"
