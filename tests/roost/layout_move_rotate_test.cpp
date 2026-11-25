// Move a panel (`layout.moveLeft`, `layout.moveRight`, `layout.moveUp`,
// `layout.moveDown`) and turn a split (`layout.rotate`). The first half is the
// pure edits on trees (no window): the swap, the step out of a split, the
// turn, the refusals, and what stays. The second half is the real window
// against a real roostd: the menu items and the commands, one write of the
// layout per edit, the refusal in the problems strip, the undo of each edit,
// and the layout that comes back after a restart.

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

LayoutLink From(const QString& from) {
    return {.From = from, .Output = "selected", .Problem = {}};
}

LayoutNode Leaf(const QString& instance, const QString& panel,
                const QMap<QString, LayoutLink>& links = {}) {
    LayoutNode node;
    node.Instance = instance;
    node.Panel = panel;
    node.Links = links;
    return node;
}

LayoutNode ListOf(const QString& instance) {
    return Leaf(instance, "sessions.list");
}

LayoutNode TermOf(const QString& instance, const QString& list = "list") {
    return Leaf(instance, "terminal.view", {{"session", From(list)}});
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

const LayoutSize REST = LayoutSize::Rest();

LayoutSize Px(int pixels) { return LayoutSize::Px(pixels); }

// The list on the left, two terminals one over the other on the right.
Layout Nested() {
    return {.Root =
                Split(true,
                      {ListOf("list"),
                       Split(false, {TermOf("terminal"), TermOf("terminal-2")},
                             {Px(200), REST})},
                      {Px(320), REST})};
}

// Every leaf of the tree: instance, panel and links, in a map by instance.
QMap<QString, LayoutNode> Leaves(const LayoutNode& node) {
    QMap<QString, LayoutNode> leaves;
    if (node.Kind == ELayoutNodeKind::Leaf) {
        leaves.insert(node.Instance, node);
        return leaves;
    }
    for (const LayoutNode& child : node.Children) {
        leaves.insert(Leaves(child));
    }
    return leaves;
}

// The panels a person can put the pointer on: the shown tab of each stack,
// every leaf of a split.
void ShownLeaves(const LayoutNode& node, QStringList& instances) {
    switch (node.Kind) {
        case ELayoutNodeKind::Leaf:
            instances.append(node.Instance);
            return;
        case ELayoutNodeKind::Split:
            for (const LayoutNode& child : node.Children) {
                ShownLeaves(child, instances);
            }
            return;
        case ELayoutNodeKind::Tabs:
            ShownLeaves(node.Children.at(static_cast<size_t>(node.Active)),
                        instances);
            return;
    }
}

// The tree reads back as itself with no problem: it is canonical.
void VerifyCanonical(const Layout& tree) {
    const LayoutParseResult parsed = ParseLayout(LayoutToJson(tree), Known());
    QVERIFY2(parsed.Problems.isEmpty(), qPrintable(parsed.Problems.join("; ")));
    QCOMPARE(parsed.Tree, tree);
}

}  // namespace

class LayoutMoveRotateTest : public LayoutWindow {
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
    void RunWritesOnce(const QString& instance, const QString& command) {
        Focus(instance);
        QVERIFY(Api_->Commands().HasLocal(command));
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Api_->Commands().Run(command);
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        // The write is the one this command made, not a later one: the
        // document is the layout of the window, and nothing follows.
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredLayout(), LayoutToJson(Shell_->CurrentLayout()), TIMEOUT_MS);
        QCOMPARE(Writes(spy), 1);
    }

    bool HasProblem(const QString& part) {
        for (const QString& problem : Problems()) {
            if (problem.contains(part)) {
                return true;
            }
        }
        return false;
    }

    static QQuickItem* InItems(QQuickItem* item, const QString& name) {
        if (item->objectName() == name) {
            return item;
        }
        for (QQuickItem* child : item->childItems()) {
            if (QQuickItem* found = InItems(child, name)) {
                return found;
            }
        }
        return nullptr;
    }

    // An item of the window by objectName, also in a menu, which hangs on the
    // overlay of the window.
    QQuickItem* Named(const QString& name) {
        if (QQuickItem* found = InItems(Window()->contentItem(), name)) {
            return found;
        }
        return Root()->findChild<QQuickItem*>(name);
    }

    // The item is there and on.
    bool On(const QString& name) {
        const QQuickItem* item = Named(name);
        return item != nullptr && item->isEnabled();
    }

    static QPoint CenterOf(QQuickItem* item) {
        return item->mapToScene(QPointF(item->width() / 2, item->height() / 2))
            .toPoint();
    }

    // The menu of the panel, opened with the right button on it.
    void OpenMenuOn(const QString& instance) {
        // The menu that was open is closed (a closing one takes no click).
        if (QObject* open = Root()->findChild<QObject*>("panelMenu")) {
            QTRY_VERIFY_WITH_TIMEOUT(!open->property("visible").toBool(),
                                     TIMEOUT_MS);
        }
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance(instance) != nullptr &&
                                     SlotByInstance(instance)->width() > 80,
                                 TIMEOUT_MS);
        QQuickItem* slot = SlotByInstance(instance);
        QTest::mouseMove(Window(), slot->mapToScene(QPointF(40, 40)).toPoint());
        QTest::mouseClick(Window(), Qt::RightButton, Qt::NoModifier,
                          slot->mapToScene(QPointF(40, 40)).toPoint());
        QObject* popup = Root()->findChild<QObject*>("panelMenu");
        QVERIFY(popup != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(popup->property("visible").toBool(),
                                 TIMEOUT_MS);
        // The items of the edits are there to be asked about.
        for (const char* item : {"menuMoveLeft", "menuMoveRight", "menuMoveUp",
                                 "menuMoveDown", "menuRotate"}) {
            QVERIFY2(Named(item) != nullptr, item);
        }
    }

    void CloseMenu() {
        QTest::keyClick(Window(), Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!Root()
                                      ->findChild<QObject*>("panelMenu")
                                      ->property("visible")
                                      .toBool(),
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

    // Opens "Move" of the menu that is open: its rows are then on the screen.
    void OpenMoveSubmenu() {
        QObject* submenu = Root()->findChild<QObject*>("menuMove");
        QVERIFY(submenu != nullptr);
        QQuickItem* row = RowOfSubmenu(submenu);
        QVERIFY(row != nullptr);
        QTest::mouseClick(Window(), Qt::LeftButton, Qt::NoModifier,
                          CenterOf(row));
        QTRY_VERIFY_WITH_TIMEOUT(submenu->property("visible").toBool(),
                                 TIMEOUT_MS);
    }

    // Opens the menu on `instance`, clicks the item (of "Move" for the moves),
    // and waits for the write.
    void ClickMenuItem(const QString& instance, const QString& item) {
        OpenMenuOn(instance);
        if (item.startsWith(QLatin1String("menuMove")) ||
            item.startsWith(QLatin1String("menuTab"))) {
            OpenMoveSubmenu();
            if (QTest::currentTestFailed()) {
                return;
            }
        }
        QQuickItem* row = Named(item);
        QVERIFY2(row != nullptr, qPrintable(item));
        QTRY_VERIFY_WITH_TIMEOUT(row->isEnabled(), TIMEOUT_MS);
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        QTest::mouseClick(Window(), Qt::LeftButton, Qt::NoModifier,
                          CenterOf(row));
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredLayout(), LayoutToJson(Shell_->CurrentLayout()), TIMEOUT_MS);
        QCOMPARE(Writes(spy), 1);
    }

    // The layout is set, and the window shows its panels.
    void StartWith(const Layout& layout) {
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Shell_->SetLayout(layout);
        QTRY_COMPARE_WITH_TIMEOUT(Shell_->CurrentLayout(), layout, TIMEOUT_MS);
        // The write is seen by the window: it is not a late one of a test.
        QTRY_VERIFY_WITH_TIMEOUT(Writes(spy) >= 1, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(StoredLayout(), LayoutToJson(layout),
                                  TIMEOUT_MS);
        for (const QString& instance : Leaves(layout.Root).keys()) {
            QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance(instance) != nullptr &&
                                         SlotByInstance(instance)->width() > 0,
                                     TIMEOUT_MS);
        }
    }

    // The window and the daemon are restarted, and the window shows the
    // layout the document kept.
    void Restart() {
        CloseWindow();
        StopDaemon();
        QVERIFY(StartDaemon());
        OpenWindow();
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

    static QPoint Where(QQuickItem* slot) { return CenterOf(slot); }

    bool Left(const QString& a, const QString& b) {
        return Where(SlotByInstance(a)).x() < Where(SlotByInstance(b)).x();
    }

    bool Above(const QString& a, const QString& b) {
        return Where(SlotByInstance(a)).y() < Where(SlotByInstance(b)).y();
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

    // ---- the pure parts ---------------------------------------------------

    void MoveSwapsWithTheNeighbourAndTheSizesGoWithThePanels() {
        const Layout layout = {
            .Root = Split(true, {ListOf("list"), TermOf("terminal")},
                          {Px(320), REST})};
        const LayoutEdit right =
            MovePanel(layout, "list", EMoveDirection::Right);
        QVERIFY(right.Refused.isEmpty());
        QVERIFY(right.Changed);
        QCOMPARE(
            right.Tree.Root,
            Split(true, {TermOf("terminal"), ListOf("list")}, {REST, Px(320)}));
        QCOMPARE(right.Focus, QString("list"));
        VerifyCanonical(right.Tree);

        // And back: left from where it went gives the document it had.
        const LayoutEdit back =
            MovePanel(right.Tree, "list", EMoveDirection::Left);
        QVERIFY(back.Changed);
        QCOMPARE(back.Tree, layout);
    }

    void MoveSwapsWithAWholeSubtreeAndInAStackedSplit() {
        const Layout layout = {
            .Root =
                Split(false,
                      {ListOf("list"),
                       Split(true, {TermOf("terminal"), TermOf("terminal-2")},
                             {Px(100), REST})},
                      {Px(50), REST})};
        const LayoutEdit down = MovePanel(layout, "list", EMoveDirection::Down);
        QVERIFY(down.Refused.isEmpty());
        QCOMPARE(down.Tree.Root,
                 Split(false,
                       {Split(true, {TermOf("terminal"), TermOf("terminal-2")},
                              {Px(100), REST}),
                        ListOf("list")},
                       {REST, Px(50)}));
        VerifyCanonical(down.Tree);
        const LayoutEdit up = MovePanel(down.Tree, "list", EMoveDirection::Up);
        QCOMPARE(up.Tree, layout);
    }

    void MoveAtTheEdgeOfASplitStepsOutIntoTheParentThatRunsThatWay() {
        // The lower terminal goes left: out of the stacked split, between the
        // list and the other terminal, which has no sibling any more.
        const LayoutEdit left =
            MovePanel(Nested(), "terminal-2", EMoveDirection::Left);
        QVERIFY(left.Refused.isEmpty());
        QVERIFY(left.Changed);
        QCOMPARE(left.Tree.Root, Split(true,
                                       {ListOf("list"), TermOf("terminal-2"),
                                        TermOf("terminal")},
                                       {Px(320), REST, REST}));
        QCOMPARE(left.Focus, QString("terminal-2"));
        VerifyCanonical(left.Tree);

        // Right from the upper terminal: after the stacked split.
        const LayoutEdit right =
            MovePanel(Nested(), "terminal", EMoveDirection::Right);
        QVERIFY(right.Refused.isEmpty());
        QCOMPARE(right.Tree.Root, Split(true,
                                        {ListOf("list"), TermOf("terminal-2"),
                                         TermOf("terminal")},
                                        {Px(320), REST, REST}));
        VerifyCanonical(right.Tree);
    }

    void MoveOutOfASplitThatRunsTheOtherWayAtItsEdge() {
        // A row in a column: the left panel goes up and the right one down,
        // each next to the row, and the row is left with one panel.
        const Layout layout = {
            .Root = Split(false,
                          {Split(true, {ListOf("list"), TermOf("terminal")},
                                 {Px(100), REST}),
                           TermOf("terminal-2")},
                          {Px(70), REST})};
        const LayoutEdit down =
            MovePanel(layout, "terminal", EMoveDirection::Down);
        QVERIFY(down.Refused.isEmpty());
        QCOMPARE(down.Tree.Root, Split(false,
                                       {ListOf("list"), TermOf("terminal"),
                                        TermOf("terminal-2")},
                                       {Px(70), REST, REST}));
        VerifyCanonical(down.Tree);
        const LayoutEdit up = MovePanel(layout, "list", EMoveDirection::Up);
        QVERIFY(up.Refused.isEmpty());
        QCOMPARE(up.Tree.Root, Split(false,
                                     {ListOf("list"), TermOf("terminal"),
                                      TermOf("terminal-2")},
                                     {REST, Px(70), REST}));
        VerifyCanonical(up.Tree);
    }

    void MoveOutOfASplitThatRunsTheSameWayAtItsEdgeIntoTheGrandparent() {
        // A row in a row: the right terminal is at the edge of the inner row
        // and goes right, after it, in the outer row that runs the same way.
        const Layout layout = {
            .Root =
                Split(true,
                      {ListOf("list"),
                       Split(true, {TermOf("terminal"), TermOf("terminal-2")},
                             {Px(200), REST})},
                      {Px(320), REST})};
        VerifyCanonical(layout);
        const LayoutEdit right =
            MovePanel(layout, "terminal-2", EMoveDirection::Right);
        QVERIFY(right.Refused.isEmpty());
        QVERIFY(right.Changed);
        QCOMPARE(right.Tree.Root, Split(true,
                                        {ListOf("list"), TermOf("terminal"),
                                         TermOf("terminal-2")},
                                        {Px(320), REST, REST}));
        QCOMPARE(right.Focus, QString("terminal-2"));
        VerifyCanonical(right.Tree);
        // The left one goes left, before the inner row, which is the one
        // panel that is left of it.
        const LayoutEdit left =
            MovePanel(layout, "terminal", EMoveDirection::Left);
        QVERIFY(left.Refused.isEmpty());
        QCOMPARE(left.Tree.Root, Split(true,
                                       {ListOf("list"), TermOf("terminal"),
                                        TermOf("terminal-2")},
                                       {Px(320), REST, REST}));
        VerifyCanonical(left.Tree);
    }

    void MoveOutOfASplitThatKeepsThreePanelsLeavesTheOthersWhereTheyAre() {
        const Layout layout = {
            .Root = Split(true,
                          {ListOf("list"),
                           Split(false,
                                 {TermOf("terminal"), TermOf("terminal-2"),
                                  TermOf("terminal-3")},
                                 {Px(100), Px(150), REST})},
                          {Px(320), REST})};
        VerifyCanonical(layout);
        // The first goes left: the column keeps its two, with their sizes.
        const LayoutEdit left =
            MovePanel(layout, "terminal", EMoveDirection::Left);
        QVERIFY(left.Refused.isEmpty());
        QCOMPARE(
            left.Tree.Root,
            Split(true,
                  {ListOf("list"), TermOf("terminal"),
                   Split(false, {TermOf("terminal-2"), TermOf("terminal-3")},
                         {Px(150), REST})},
                  {Px(320), REST, REST}));
        VerifyCanonical(left.Tree);
        // The middle one goes right: after the column, which keeps the others.
        const LayoutEdit right =
            MovePanel(layout, "terminal-2", EMoveDirection::Right);
        QVERIFY(right.Refused.isEmpty());
        QCOMPARE(right.Tree.Root,
                 Split(true,
                       {ListOf("list"),
                        Split(false, {TermOf("terminal"), TermOf("terminal-3")},
                              {Px(100), REST}),
                        TermOf("terminal-2")},
                       {Px(320), REST, REST}));
        VerifyCanonical(right.Tree);
    }

    void LeavingWithTheOnlyRestGivesTheRestToTheLastPanelThatIsLeft() {
        // The last panel of the column has the rest and the others have
        // pixels. When it goes, the split would have no rest: the last one
        // that stays takes it (what Tidy does), and keeps no pixel size.
        const Layout layout = {
            .Root = Split(true,
                          {ListOf("list"),
                           Split(false,
                                 {TermOf("terminal"), TermOf("terminal-2"),
                                  TermOf("terminal-3")},
                                 {Px(100), Px(150), REST})},
                          {Px(320), REST})};
        const LayoutEdit edit =
            MovePanel(layout, "terminal-3", EMoveDirection::Right);
        QVERIFY(edit.Refused.isEmpty());
        QCOMPARE(edit.Tree.Root,
                 Split(true,
                       {ListOf("list"),
                        Split(false, {TermOf("terminal"), TermOf("terminal-2")},
                              {Px(100), REST}),
                        TermOf("terminal-3")},
                       {Px(320), REST, REST}));
        VerifyCanonical(edit.Tree);
    }

    void MoveSwapsWithANeighbourThatIsAStackOfTabs() {
        const Layout layout = {
            .Root = Split(true,
                          {Tabs({ListOf("list"), TermOf("terminal")}, 1),
                           TermOf("terminal-2")},
                          {Px(300), REST})};
        const LayoutEdit edit =
            MovePanel(layout, "terminal-2", EMoveDirection::Left);
        QVERIFY(edit.Refused.isEmpty());
        QCOMPARE(edit.Tree.Root,
                 Split(true,
                       {TermOf("terminal-2"),
                        Tabs({ListOf("list"), TermOf("terminal")}, 1)},
                       {REST, Px(300)}));
        VerifyCanonical(edit.Tree);
        QCOMPARE(MovePanel(edit.Tree, "terminal-2", EMoveDirection::Right).Tree,
                 layout);
    }

    void MoveKeepsTheInstancesLinksStateAndTheOtherTabs() {
        Layout layout = {
            .Root = Split(true,
                          {Tabs({ListOf("list"), TermOf("terminal")}, 1),
                           TermOf("terminal-2")},
                          {Px(300), REST})};
        layout.ListsHidden = true;
        const QMap<QString, LayoutNode> before = Leaves(layout.Root);
        const LayoutEdit edit =
            MovePanel(layout, "terminal-2", EMoveDirection::Left);
        QVERIFY(edit.Refused.isEmpty());
        QVERIFY(edit.Changed);
        QCOMPARE(Leaves(edit.Tree.Root), before);
        QCOMPARE(CountLeaves(edit.Tree.Root), 3);
        // The stack is moved whole: the tab that was shown is still shown.
        QCOMPARE(edit.Tree.Root.Children[1].Kind, ELayoutNodeKind::Tabs);
        QCOMPARE(edit.Tree.Root.Children[1].Active, 1);
        QVERIFY(edit.Tree.ListsHidden);
        VerifyCanonical(edit.Tree);
    }

    void MoveIsRefusedWithAReasonAndLeavesTheTree() {
        const Layout row = {.Root = Split(true,
                                          {ListOf("list"), TermOf("terminal")},
                                          {Px(320), REST})};
        const Layout single = {.Root = ListOf("list")};
        const Layout tabbed = {
            .Root = Split(true,
                          {Tabs({ListOf("list"), TermOf("terminal")}, 0),
                           TermOf("terminal-2")},
                          {Px(300), REST})};
        const Layout column = {
            .Root = Split(false,
                          {Split(true, {ListOf("list"), TermOf("terminal")},
                                 {Px(100), REST}),
                           TermOf("terminal-2")},
                          {Px(70), REST})};
        struct Case {
            const Layout* Tree;
            QString Instance;
            EMoveDirection Direction;
            QString Reason;
        };
        const std::vector<Case> cases = {
            {&row, "terminal", EMoveDirection::Right,
             "terminal cannot move right: no split around it has room to its "
             "right"},
            {&row, "list", EMoveDirection::Left,
             "list cannot move left: no split around it has room to its left"},
            {&row, "list", EMoveDirection::Up,
             "list cannot move up: no split around it has room above it"},
            {&row, "list", EMoveDirection::Down,
             "list cannot move down: no split around it has room below it"},
            {&single, "list", EMoveDirection::Right,
             "list is the only panel: there is nothing to move it past"},
            {&tabbed, "terminal", EMoveDirection::Left,
             "terminal is in tabs: take it out of tabs to move it"},
            {&column, "list", EMoveDirection::Left,
             "list cannot move left: no split around it has room to its left"},
            {&row, "nothing", EMoveDirection::Left,
             "there is no panel nothing to move"},
        };
        for (const Case& c : cases) {
            const LayoutEdit edit = MovePanel(*c.Tree, c.Instance, c.Direction);
            QCOMPARE(edit.Refused, c.Reason);
            QVERIFY(!edit.Changed);
            QCOMPARE(edit.Tree, *c.Tree);
            QVERIFY(edit.Focus.isEmpty());
        }
    }

    void RotateTurnsTheSplitAndTheSizesCarryOver() {
        const Layout layout = {
            .Root = Split(true, {ListOf("list"), TermOf("terminal")},
                          {Px(320), REST})};
        const LayoutEdit turned = RotateSplit(layout, "terminal");
        QVERIFY(turned.Refused.isEmpty());
        QVERIFY(turned.Changed);
        QCOMPARE(turned.Tree.Root,
                 Split(false, {ListOf("list"), TermOf("terminal")},
                       {Px(320), REST}));
        QCOMPARE(turned.Focus, QString("terminal"));
        VerifyCanonical(turned.Tree);
        // Twice is the document that was.
        QCOMPARE(RotateSplit(turned.Tree, "list").Tree, layout);
    }

    void RotateTurnsOnlyTheSplitThatHoldsThePanel() {
        const LayoutEdit edit = RotateSplit(Nested(), "terminal-2");
        QVERIFY(edit.Refused.isEmpty());
        QCOMPARE(edit.Tree.Root,
                 Split(true,
                       {ListOf("list"),
                        Split(true, {TermOf("terminal"), TermOf("terminal-2")},
                              {Px(200), REST})},
                       {Px(320), REST}));
        VerifyCanonical(edit.Tree);
        // The list is in the outer split: it turns that one.
        const LayoutEdit outer = RotateSplit(Nested(), "list");
        QCOMPARE(outer.Tree.Root,
                 Split(false,
                       {ListOf("list"),
                        Split(false, {TermOf("terminal"), TermOf("terminal-2")},
                              {Px(200), REST})},
                       {Px(320), REST}));
    }

    void RotateThatLeavesASplitInASplitOfTheSameWayIsValid() {
        const Layout layout = {
            .Root = Split(
                true,
                {ListOf("list"),
                 Split(
                     false,
                     {TermOf("terminal"),
                      Split(true, {TermOf("terminal-2"), TermOf("terminal-3")},
                            {Px(150), REST})},
                     {Px(200), REST})},
                {Px(320), REST})};
        VerifyCanonical(layout);
        // The inner row turns into a column in a column.
        const LayoutEdit inner = RotateSplit(layout, "terminal-2");
        QVERIFY(inner.Refused.isEmpty());
        QCOMPARE(inner.Tree.Root.Children[1].Children[1],
                 Split(false, {TermOf("terminal-2"), TermOf("terminal-3")},
                       {Px(150), REST}));
        VerifyCanonical(inner.Tree);
        // The column turns into a row in a row.
        const LayoutEdit middle = RotateSplit(layout, "terminal");
        QVERIFY(middle.Refused.isEmpty());
        QVERIFY(middle.Tree.Root.Children[1].Horizontal);
        QVERIFY(middle.Tree.Root.Children[1].Children[1].Horizontal);
        VerifyCanonical(middle.Tree);
        QCOMPARE(RotateSplit(middle.Tree, "terminal").Tree, layout);
    }

    void RotateOfAPanelInTabsTurnsTheSplitThatHoldsTheStack() {
        Layout layout = {
            .Root = Split(true,
                          {Tabs({ListOf("list"), TermOf("terminal")}, 1),
                           TermOf("terminal-2")},
                          {Px(300), REST})};
        layout.ListsHidden = true;
        const QMap<QString, LayoutNode> before = Leaves(layout.Root);
        const LayoutEdit edit = RotateSplit(layout, "terminal");
        QVERIFY(edit.Refused.isEmpty());
        QVERIFY(!edit.Tree.Root.Horizontal);
        QCOMPARE(edit.Tree.Root.Sizes, layout.Root.Sizes);
        QCOMPARE(edit.Tree.Root.Children, layout.Root.Children);
        QCOMPARE(Leaves(edit.Tree.Root), before);
        QVERIFY(edit.Tree.ListsHidden);
        VerifyCanonical(edit.Tree);
    }

    void RotateIsRefusedForTheOnlyPanelAndForAnUnknownOne() {
        const Layout single = {.Root = ListOf("list")};
        const LayoutEdit only = RotateSplit(single, "list");
        QCOMPARE(only.Refused,
                 QString("list is the only panel: there is no split to turn"));
        QVERIFY(!only.Changed);
        QCOMPARE(only.Tree, single);

        const LayoutEdit none = RotateSplit(Nested(), "nothing");
        QCOMPARE(none.Refused,
                 QString("there is no panel nothing to turn the split of"));
        QVERIFY(!none.Changed);
        QCOMPARE(none.Tree, Nested());

        // A stack of tabs that is the whole layout has no split either.
        const Layout tabs = {.Root =
                                 Tabs({ListOf("list"), TermOf("terminal")}, 0)};
        QVERIFY(!RotateSplit(tabs, "list").Refused.isEmpty());
    }

    // ---- the window -------------------------------------------------------

    // The items of the menu say what the edits do: an item is on exactly when
    // the edit is not refused, for every panel of trees of several shapes.
    void TheMenuItemsAreOnWhereTheEditsWork() {
        BootWith(Nested());
        if (QTest::currentTestFailed()) {
            return;
        }
        const struct {
            const char* Item;
            EMoveDirection Direction;
        } moves[] = {{"menuMoveLeft", EMoveDirection::Left},
                     {"menuMoveRight", EMoveDirection::Right},
                     {"menuMoveUp", EMoveDirection::Up},
                     {"menuMoveDown", EMoveDirection::Down}};
        const std::vector<Layout> trees = {
            Nested(),
            // A row of two: the right one can only go left, and turn.
            {.Root = Split(true, {ListOf("list"), TermOf("terminal")},
                           {Px(320), REST})},
            // Three levels deep.
            {.Root = Split(
                 true,
                 {ListOf("list"),
                  Split(
                      false,
                      {TermOf("terminal"),
                       Split(true, {TermOf("terminal-2"), TermOf("terminal-3")},
                             {Px(100), REST})},
                      {Px(100), REST})},
                 {Px(200), REST})},
            // A row in a row, and a column in a row in a column.
            {.Root =
                 Split(true,
                       {ListOf("list"),
                        Split(true, {TermOf("terminal"), TermOf("terminal-2")},
                              {Px(100), REST})},
                       {Px(200), REST})},
            {.Root = Split(false,
                           {Split(true, {ListOf("list"), TermOf("terminal")},
                                  {Px(100), REST}),
                            TermOf("terminal-2")},
                           {Px(100), REST})},
            // The grandparent is a stack of tabs: the split is the shown tab.
            {.Root = Tabs({Split(false, {ListOf("list"), TermOf("terminal")},
                                 {Px(100), REST}),
                           TermOf("terminal-2")},
                          0)},
            // The panels are in a stack of tabs next to a panel; the one
            // tab is shown, and a leaf in the stack.
            {.Root = Split(true,
                           {Tabs({ListOf("list"), TermOf("terminal")}, 1),
                            TermOf("terminal-2")},
                           {Px(300), REST})},
            // The only panel.
            {.Root = ListOf("list")},
        };
        for (const Layout& tree : trees) {
            StartWith(tree);
            if (QTest::currentTestFailed()) {
                return;
            }
            QStringList shown;
            ShownLeaves(tree.Root, shown);
            for (const QString& instance : shown) {
                OpenMenuOn(instance);
                for (const auto& move : moves) {
                    QQuickItem* row = Named(move.Item);
                    QVERIFY2(row != nullptr, move.Item);
                    const bool works = MovePanel(tree, instance, move.Direction)
                                           .Refused.isEmpty();
                    QVERIFY2(
                        row->isEnabled() == works,
                        qPrintable(
                            QStringLiteral("%1 %2 in %3")
                                .arg(
                                    instance, move.Item,
                                    QString::fromUtf8(
                                        QJsonDocument(LayoutToJson(tree))
                                            .toJson(QJsonDocument::Compact)))));
                }
                QQuickItem* rotate = Named("menuRotate");
                QVERIFY(rotate != nullptr);
                const bool turns =
                    RotateSplit(tree, instance).Refused.isEmpty();
                QVERIFY2(rotate->isEnabled() == turns,
                         qPrintable(instance + " menuRotate"));
                CloseMenu();
            }
        }
        // The only panel has nothing to turn.
        StartWith(trees.back());
        OpenMenuOn("list");
        QVERIFY(!On("menuRotate"));
        QVERIFY(!On("menuMoveLeft"));
    }

    void TheMenuEditsKeepThePanelsThatCollapseAndThatTurn() {
        BootWith(Nested());
        if (QTest::currentTestFailed()) {
            return;
        }
        const QStringList instances = {"list", "terminal", "terminal-2"};
        QMap<QString, QQuickItem*> before;
        for (const QString& instance : instances) {
            before.insert(instance, PanelByInstance(instance));
            QVERIFY(before.value(instance) != nullptr);
        }
        // The lower terminal goes out to the row: the column that held it
        // is gone, and its other panel is where the column was.
        ClickMenuItem("terminal-2", "menuMoveLeft");
        QCOMPARE(
            Shell_->CurrentLayout().Root,
            Split(true,
                  {ListOf("list"), TermOf("terminal-2"), TermOf("terminal")},
                  {Px(320), REST, REST}));
        for (const QString& instance : instances) {
            QTRY_COMPARE_WITH_TIMEOUT(PanelByInstance(instance),
                                      before.value(instance), TIMEOUT_MS);
        }
        // The row turns into a column.
        ClickMenuItem("terminal", "menuRotate");
        QVERIFY(!Shell_->CurrentLayout().Root.Horizontal);
        QTRY_VERIFY_WITH_TIMEOUT(Above("list", "terminal-2"), TIMEOUT_MS);
        for (const QString& instance : instances) {
            QTRY_COMPARE_WITH_TIMEOUT(PanelByInstance(instance),
                                      before.value(instance), TIMEOUT_MS);
        }
        QVERIFY(Problems().isEmpty());
    }

    void TheMenuItemMovesThePanelAndUndoBringsItBack() {
        BootWith(Nested());
        if (QTest::currentTestFailed()) {
            return;
        }
        QQuickItem* terminalPanel = PanelByInstance("terminal");
        QQuickItem* otherPanel = PanelByInstance("terminal-2");
        QVERIFY(terminalPanel != nullptr && otherPanel != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(Above("terminal", "terminal-2"), TIMEOUT_MS);

        ClickMenuItem("terminal", "menuMoveDown");
        const Layout moved = {
            .Root =
                Split(true,
                      {ListOf("list"),
                       Split(false, {TermOf("terminal-2"), TermOf("terminal")},
                             {REST, Px(200)})},
                      {Px(320), REST})};
        QCOMPARE(Shell_->CurrentLayout(), moved);
        QCOMPARE(StoredLayout(), LayoutToJson(moved));
        QTRY_VERIFY_WITH_TIMEOUT(Above("terminal-2", "terminal"), TIMEOUT_MS);
        QVERIFY(Problems().isEmpty());
        // The panels are the same ones: not made again.
        QCOMPARE(PanelByInstance("terminal"), terminalPanel);
        QCOMPARE(PanelByInstance("terminal-2"), otherPanel);
        // The keyboard stays in the panel that was moved.
        QTRY_VERIFY_WITH_TIMEOUT(
            SlotByInstance("terminal") != nullptr &&
                SlotByInstance("terminal")->hasActiveFocus(),
            TIMEOUT_MS);

        ClickMenuItem("terminal", "menuUndo");
        QCOMPARE(Shell_->CurrentLayout(), Nested());
        QCOMPARE(StoredLayout(), LayoutToJson(Nested()));
        QTRY_VERIFY_WITH_TIMEOUT(Above("terminal", "terminal-2"), TIMEOUT_MS);
    }

    void TheCommandsMoveInEachDirectionOneWriteEachAndUndoEach() {
        BootWith(Nested());
        if (QTest::currentTestFailed()) {
            return;
        }
        // The steps: the command, the panel that has the keyboard, and the
        // layout after it.
        struct Step {
            const char* Command;
            const char* Instance;
            Layout After;
        };
        const std::vector<Step> steps = {
            {"layout.moveLeft",
             "terminal-2",
             {.Root = Split(
                  true,
                  {ListOf("list"), TermOf("terminal-2"), TermOf("terminal")},
                  {Px(320), REST, REST})}},
            {"layout.moveRight",
             "terminal-2",
             {.Root = Split(
                  true,
                  {ListOf("list"), TermOf("terminal"), TermOf("terminal-2")},
                  {Px(320), REST, REST})}},
            {"layout.moveLeft",
             "terminal-2",
             {.Root = Split(
                  true,
                  {ListOf("list"), TermOf("terminal-2"), TermOf("terminal")},
                  {Px(320), REST, REST})}},
            {"layout.moveLeft",
             "terminal-2",
             {.Root = Split(
                  true,
                  {TermOf("terminal-2"), ListOf("list"), TermOf("terminal")},
                  {REST, Px(320), REST})}},
        };
        std::vector<Layout> before;
        for (const Step& step : steps) {
            before.push_back(Shell_->CurrentLayout());
            RunWritesOnce(step.Instance, step.Command);
            QCOMPARE(Shell_->CurrentLayout(), step.After);
            QCOMPARE(StoredLayout(), LayoutToJson(step.After));
            QVERIFY(Problems().isEmpty());
        }
        for (auto at = before.rbegin(); at != before.rend(); ++at) {
            RunWritesOnce("terminal-2", "layout.undo");
            QCOMPARE(Shell_->CurrentLayout(), *at);
            QCOMPARE(StoredLayout(), LayoutToJson(*at));
        }
    }

    void TheCommandsMoveUpAndDownThroughAColumnOfRows() {
        // A row over a terminal: the terminal of the row goes down next to
        // the row, and up it swaps with the list that stays.
        const Layout column = {
            .Root = Split(false,
                          {Split(true, {ListOf("list"), TermOf("terminal")},
                                 {Px(320), REST}),
                           TermOf("terminal-2")},
                          {Px(300), REST})};
        BootWith(column);
        if (QTest::currentTestFailed()) {
            return;
        }
        RunWritesOnce("terminal", "layout.moveDown");
        const Layout down = {.Root = Split(false,
                                           {ListOf("list"), TermOf("terminal"),
                                            TermOf("terminal-2")},
                                           {Px(300), REST, REST})};
        QCOMPARE(Shell_->CurrentLayout(), down);
        QCOMPARE(StoredLayout(), LayoutToJson(down));
        QTRY_VERIFY_WITH_TIMEOUT(
            Above("list", "terminal") && Above("terminal", "terminal-2"),
            TIMEOUT_MS);

        RunWritesOnce("terminal", "layout.moveUp");
        const Layout up = {.Root = Split(false,
                                         {TermOf("terminal"), ListOf("list"),
                                          TermOf("terminal-2")},
                                         {REST, Px(300), REST})};
        QCOMPARE(Shell_->CurrentLayout(), up);
        QCOMPARE(StoredLayout(), LayoutToJson(up));
        QTRY_VERIFY_WITH_TIMEOUT(Above("terminal", "list"), TIMEOUT_MS);

        RunWritesOnce("terminal", "layout.undo");
        QCOMPARE(Shell_->CurrentLayout(), down);
        RunWritesOnce("terminal", "layout.undo");
        QCOMPARE(Shell_->CurrentLayout(), column);
    }

    void AMoveThatCannotBeDoneShowsItsReasonAndWritesNothing() {
        BootWith(Nested());
        if (QTest::currentTestFailed()) {
            return;
        }
        Focus("list");
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Api_->Commands().Run("layout.moveLeft");
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem("list cannot move left: no split "
                                            "around it has room to its left"),
                                 TIMEOUT_MS);
        QCOMPARE(Shell_->CurrentLayout(), Nested());
        // A refusal is no edit: nothing to undo.
        Api_->Commands().Run("layout.undo");
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem("nothing to undo"), TIMEOUT_MS);
        // The edit that works clears the reason, and it is the only write.
        Focus("terminal");
        Api_->Commands().Run("layout.rotate");
        QTRY_VERIFY_WITH_TIMEOUT(!HasProblem("nothing to undo"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        QVERIFY(Problems().isEmpty());
    }

    void AMoveOfAPanelInTabsAndOfTheOnlyPanelIsRefused() {
        const Layout tabbed = {
            .Root = Split(true,
                          {Tabs({ListOf("list"), TermOf("terminal")}, 1),
                           TermOf("terminal-2")},
                          {Px(300), REST})};
        BootWith(tabbed);
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenMenuOn("terminal-2");
        QVERIFY(!On("menuMoveRight"));
        QVERIFY(On("menuMoveLeft"));
        CloseMenu();
        // A panel of the stack: the item is off, the command says why.
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("terminal") != nullptr &&
                                     SlotByInstance("terminal")->width() > 80,
                                 TIMEOUT_MS);
        OpenMenuOn("terminal");
        QVERIFY(!On("menuMoveLeft"));
        QVERIFY(!On("menuMoveRight"));
        QVERIFY(!On("menuMoveUp"));
        QVERIFY(!On("menuMoveDown"));
        CloseMenu();
        Focus("terminal");
        Api_->Commands().Run("layout.moveLeft");
        QTRY_VERIFY_WITH_TIMEOUT(
            HasProblem("terminal is in tabs: take it out of tabs to move it"),
            TIMEOUT_MS);
        QCOMPARE(Shell_->CurrentLayout(), tabbed);
    }

    void AMovedPanelIsStillThereAfterARestart() {
        BootWith(Nested());
        if (QTest::currentTestFailed()) {
            return;
        }
        RunWritesOnce("terminal-2", "layout.moveLeft");
        const Layout moved = Shell_->CurrentLayout();
        QCOMPARE(moved.Root, Split(true,
                                   {ListOf("list"), TermOf("terminal-2"),
                                    TermOf("terminal")},
                                   {Px(320), REST, REST}));

        Restart();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Shell_->CurrentLayout(), moved, TIMEOUT_MS);
        QCOMPARE(StoredLayout(), LayoutToJson(moved));
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal") != nullptr &&
                                     SlotByInstance("terminal-2") != nullptr &&
                                     SlotByInstance("terminal")->width() > 0,
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Left("terminal-2", "terminal"), TIMEOUT_MS);
        QVERIFY(Problems().isEmpty());
    }

    void TheMenuItemTurnsTheSplitAndUndoTurnsItBack() {
        const Layout row = {.Root = Split(true,
                                          {ListOf("list"), TermOf("terminal")},
                                          {Px(320), REST})};
        BootWith(row);
        if (QTest::currentTestFailed()) {
            return;
        }
        QQuickItem* listPanel = PanelByInstance("list");
        QQuickItem* terminalPanel = PanelByInstance("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(Left("list", "terminal"), TIMEOUT_MS);

        ClickMenuItem("terminal", "menuRotate");
        const Layout turned = {
            .Root = Split(false, {ListOf("list"), TermOf("terminal")},
                          {Px(320), REST})};
        QCOMPARE(Shell_->CurrentLayout(), turned);
        QCOMPARE(StoredLayout(), LayoutToJson(turned));
        QTRY_VERIFY_WITH_TIMEOUT(Above("list", "terminal"), TIMEOUT_MS);
        // The list is 320 px high now, as it was wide.
        QTRY_COMPARE_WITH_TIMEOUT(
            static_cast<int>(SlotByInstance("list")->height()), 320,
            TIMEOUT_MS);
        QVERIFY(Problems().isEmpty());
        QCOMPARE(PanelByInstance("list"), listPanel);
        QCOMPARE(PanelByInstance("terminal"), terminalPanel);

        ClickMenuItem("terminal", "menuUndo");
        QCOMPARE(Shell_->CurrentLayout(), row);
        QCOMPARE(StoredLayout(), LayoutToJson(row));
        QTRY_VERIFY_WITH_TIMEOUT(Left("list", "terminal"), TIMEOUT_MS);
    }

    void TheRotateCommandTurnsTheSplitThatHoldsTheFocusedPanel() {
        BootWith(Nested());
        if (QTest::currentTestFailed()) {
            return;
        }
        RunWritesOnce("terminal", "layout.rotate");
        const Layout inner = {
            .Root =
                Split(true,
                      {ListOf("list"),
                       Split(true, {TermOf("terminal"), TermOf("terminal-2")},
                             {Px(200), REST})},
                      {Px(320), REST})};
        QCOMPARE(Shell_->CurrentLayout(), inner);
        QCOMPARE(StoredLayout(), LayoutToJson(inner));
        QTRY_VERIFY_WITH_TIMEOUT(Left("terminal", "terminal-2"), TIMEOUT_MS);

        RunWritesOnce("list", "layout.rotate");
        QCOMPARE(Shell_->CurrentLayout().Root.Horizontal, false);
        QTRY_VERIFY_WITH_TIMEOUT(Above("list", "terminal"), TIMEOUT_MS);

        RunWritesOnce("list", "layout.undo");
        QCOMPARE(Shell_->CurrentLayout(), inner);
        RunWritesOnce("list", "layout.undo");
        QCOMPARE(Shell_->CurrentLayout(), Nested());
    }

    void ATurnedSplitIsStillTurnedAfterARestart() {
        BootWith(Nested());
        if (QTest::currentTestFailed()) {
            return;
        }
        RunWritesOnce("list", "layout.rotate");
        const Layout turned = Shell_->CurrentLayout();
        QVERIFY(!turned.Root.Horizontal);

        Restart();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Shell_->CurrentLayout(), turned, TIMEOUT_MS);
        QCOMPARE(StoredLayout(), LayoutToJson(turned));
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal") != nullptr &&
                                     SlotByInstance("list") != nullptr &&
                                     SlotByInstance("terminal")->width() > 0,
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Above("list", "terminal"), TIMEOUT_MS);
        QVERIFY(Problems().isEmpty());
    }

    void TurningWithNoSplitShowsTheReasonAndWritesNothing() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        // One panel left: close the terminal.
        RunWritesOnce("terminal", "layout.close");
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal") == nullptr,
                                 TIMEOUT_MS);
        OpenMenuOn("list");
        QVERIFY(!On("menuRotate"));
        QVERIFY(!On("menuMoveLeft"));
        QVERIFY(!On("menuMoveRight"));
        CloseMenu();
        const Layout single = Shell_->CurrentLayout();
        Focus("list");
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Api_->Commands().Run("layout.rotate");
        QTRY_VERIFY_WITH_TIMEOUT(
            HasProblem("list is the only panel: there is no split to turn"),
            TIMEOUT_MS);
        Api_->Commands().Run("layout.moveRight");
        QTRY_VERIFY_WITH_TIMEOUT(
            HasProblem("list is the only panel: there is nothing to move it "
                       "past"),
            TIMEOUT_MS);
        QCOMPARE(Shell_->CurrentLayout(), single);
        // Undo brings the terminal back: the only write after the two
        // refusals, so they wrote nothing.
        Api_->Commands().Run("layout.undo");
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        QCOMPARE(CountLeaves(Shell_->CurrentLayout().Root), 2);
    }
};

QTEST_MAIN(LayoutMoveRotateTest)
#include "layout_move_rotate_test.moc"
