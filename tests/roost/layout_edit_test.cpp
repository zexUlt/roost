// Editing the layout: split, tabs, close, links. The first half is the pure
// edits of layout_commands.hpp on trees (no window); the second half is the
// real window against a real roostd: each command changes the layout and the
// document that roostd keeps exactly once, the link editor offers only ports
// of the type of the input, closing leaves no dangling link and never the last
// panel, and an edited layout comes back after a restart.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlExtensionPlugin>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>

#include <optional>

#include "helpers/layout_window.hpp"
#include "helpers/scoped_env.hpp"
#include "roost/layout_commands.hpp"
#include "terminal/session_pane.hpp"

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
        {"probe.gauge",
         {.Inputs = {},
          .Outputs = {{"level", "Level"}, {"picked", "SessionRef"}}}},
        {"probe.meter", {.Inputs = {{"level", "Level"}}, .Outputs = {}}},
        {"probe.mixer",
         {.Inputs = {{"session", "SessionRef"}, {"level", "Level"}},
          .Outputs = {{"mixed", "SessionRef"}}}},
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

LayoutLink From(const QString& from, const QString& output) {
    return {.From = from, .Output = output, .Problem = {}};
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

Layout Tree(LayoutNode root) { return {.Root = std::move(root)}; }

// The leaves of a tree, in document order, as "instance" strings.
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

const LayoutNode& Leaf(const Layout& layout, const QString& instance) {
    const LayoutNode* leaf = FindLeaf(layout.Root, instance);
    if (leaf == nullptr) {
        qFatal("no leaf %s in the tree", qPrintable(instance));
    }
    return *leaf;
}

// The window's own default shape: list and terminal side by side.
Layout ListAndTerminal() {
    return Tree(Split(true,
                      {Leaf("list", "sessions.list"),
                       Leaf("terminal", "terminal.view",
                            {{"session", From("list", "selected")}})},
                      {LayoutSize::Px(320), LayoutSize::Rest()}));
}

// What a stored layout is read as: no problem, and the same tree.
void VerifyReadsBack(const Layout& layout) {
    const LayoutParseResult parsed = ParseLayout(LayoutToJson(layout), Known());
    QVERIFY2(parsed.Problems.isEmpty(), qPrintable(parsed.Problems.join("; ")));
    QCOMPARE(parsed.Tree, layout);
}

}  // namespace

class LayoutEditTest : public LayoutWindow {
    Q_OBJECT

private:
    std::optional<ScopedEnv> Home_;
    std::optional<ScopedEnv> Socket_;
    std::optional<ScopedEnv> Config_;

    // The events `store.changed` of the layout document the window has had
    // since the spy was made: each is one write of roostd.
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

    // Runs the command of the window with the keyboard in `instance`, and
    // checks that roostd wrote the layout once, and that what it keeps is the
    // layout the window shows.
    void RunWritesOnce(const QString& instance, const QString& command,
                       const QJsonValue& args = QJsonValue::Undefined) {
        Focus(instance);
        QVERIFY(Api_->Commands().HasLocal(command));
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Api_->Commands().Run(command, args);
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        // No second write follows.
        QTest::qWait(200);
        QCOMPARE(Writes(spy), 1);
        QCOMPARE(StoredLayout(), LayoutToJson(Shell_->CurrentLayout()));
    }

    // Runs the command with the keyboard in `instance`, and waits for nothing.
    void RunLocal(const QString& instance, const QString& command) {
        Focus(instance);
        QVERIFY(Api_->Commands().HasLocal(command));
        Api_->Commands().Run(command);
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

    // An item of the window by objectName, also in the content of a dialog,
    // which hangs on the overlay of the window and not on the item tree.
    QQuickItem* Named(const QString& name) {
        if (QQuickItem* found = InItems(Window()->contentItem(), name)) {
            return found;
        }
        return Root()->findChild<QQuickItem*>(name);
    }

    // A popup or a dialog is not an item.
    QObject* Popup(const QString& name) {
        return Root()->findChild<QObject*>(name);
    }

    // The labels a combo box of the link editor offers.
    QStringList Offered(const QString& input) {
        QQuickItem* box = Named("linkInput_" + input);
        QStringList labels;
        if (box == nullptr) {
            return labels;
        }
        const int count = box->property("count").toInt();
        for (int i = 0; i < count; ++i) {
            QString label;
            QMetaObject::invokeMethod(
                box, "textAt", Q_RETURN_ARG(QString, label), Q_ARG(int, i));
            labels.append(label);
        }
        return labels;
    }

    // The user picks the entry of the combo box.
    void Choose(const QString& input, const QString& label) {
        QQuickItem* box = Named("linkInput_" + input);
        QVERIFY(box != nullptr);
        const QStringList offered = Offered(input);
        const int index = static_cast<int>(offered.indexOf(label));
        QVERIFY2(index >= 0, qPrintable(offered.join(", ")));
        QVERIFY(box->setProperty("currentIndex", index));
        QVERIFY(QMetaObject::invokeMethod(box, "activated", Q_ARG(int, index)));
    }

    // A plugin of the case with two more panel types: `gauge` gives a Level
    // and a SessionRef, `meter` takes a Level.
    void WriteProbePlugin() {
        const QString dir = Dir_->filePath("plugins/probe");
        QVERIFY(QDir().mkpath(dir));
        const auto write = [&dir](const char* name, const QByteArray& text) {
            QFile file(dir + "/" + name);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write(text) > 0);
        };
        write("plugin.json", R"({
  "id": "probe", "version": "0.1.0", "api": "0",
  "contributes": {"panels": [
    {"id": "gauge", "qml": "Gauge.qml", "inputs": {},
     "outputs": {"level": "Level", "picked": "SessionRef"}},
    {"id": "meter", "qml": "Meter.qml", "inputs": {"level": "Level"},
     "outputs": {}}]}})");
        write("Gauge.qml", R"(import QtQuick
Item { property var level: 3; property var picked: null })");
        write("Meter.qml", R"(import QtQuick
Item { property var level: null })");
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

    // ---- the pure edits ---------------------------------------------------

    void SplitMakesANewInstanceOfTheTypeWithNoLinks() {
        const LayoutEdit edit =
            SplitPanel(ListAndTerminal(), "terminal", true, {}, Known());
        QVERIFY2(edit.Refused.isEmpty(), qPrintable(edit.Refused));
        QVERIFY(edit.Changed);
        // The new half is after the source, in the same split; the list keeps
        // its width and both halves share the rest.
        QCOMPARE(Instances(edit.Tree.Root),
                 (QStringList{"list", "terminal", "terminal-2"}));
        QCOMPARE(edit.Tree.Root.Sizes,
                 (std::vector{LayoutSize::Px(320), LayoutSize::Rest(),
                              LayoutSize::Rest()}));
        const LayoutNode& added = Leaf(edit.Tree, "terminal-2");
        QCOMPARE(added.Panel, QString("terminal.view"));
        // The new half is not fed: a copy of the links of the source would
        // take its session. The source keeps its links.
        QVERIFY(added.Links.isEmpty());
        // The source is what it was, and the focus stays on it.
        QCOMPARE(Leaf(edit.Tree, "terminal"),
                 Leaf(ListAndTerminal(), "terminal"));
        QCOMPARE(edit.Focus, QString("terminal"));
        VerifyReadsBack(edit.Tree);
    }

    void SplitAcrossTheDirectionOfTheParentWrapsTheSource() {
        const LayoutEdit edit =
            SplitPanel(ListAndTerminal(), "terminal", false, {}, Known());
        QVERIFY(edit.Refused.isEmpty());
        QCOMPARE(edit.Tree.Root.Children.size(), size_t{2});
        const LayoutNode& wrapped = edit.Tree.Root.Children[1];
        QCOMPARE(wrapped.Kind, ELayoutNodeKind::Split);
        QVERIFY(!wrapped.Horizontal);
        QCOMPARE(Instances(wrapped), (QStringList{"terminal", "terminal-2"}));
        QCOMPARE(wrapped.Sizes,
                 (std::vector{LayoutSize::Rest(), LayoutSize::Rest()}));
        // The place of the source in its parent is unchanged.
        QCOMPARE(edit.Tree.Root.Sizes, ListAndTerminal().Root.Sizes);
        VerifyReadsBack(edit.Tree);
    }

    void SplitOfTheOnlyPanelAndOfATabMakesASplitThere() {
        const LayoutEdit alone = SplitPanel(Tree(Leaf("list", "sessions.list")),
                                            "list", false, {}, Known());
        QVERIFY(alone.Refused.isEmpty());
        QCOMPARE(alone.Tree.Root.Kind, ELayoutNodeKind::Split);
        QVERIFY(!alone.Tree.Root.Horizontal);
        QCOMPARE(Instances(alone.Tree.Root), (QStringList{"list", "list-2"}));

        const Layout stack = Tree(Tabs(
            {Leaf("list", "sessions.list"), Leaf("gauge", "probe.gauge")}, 1));
        const LayoutEdit inTab = SplitPanel(stack, "gauge", true, {}, Known());
        QVERIFY(inTab.Refused.isEmpty());
        QCOMPARE(inTab.Tree.Root.Kind, ELayoutNodeKind::Tabs);
        QCOMPARE(inTab.Tree.Root.Active, 1);
        QCOMPARE(inTab.Tree.Root.Children[1].Kind, ELayoutNodeKind::Split);
        QCOMPARE(Instances(inTab.Tree.Root.Children[1]),
                 (QStringList{"gauge", "gauge-2"}));
    }

    void SplitWithAnotherTypeHasNoLinksEither() {
        const Layout before = Tree(Split(
            true,
            {Leaf("list", "sessions.list"), Leaf("gauge", "probe.gauge"),
             Leaf("terminal", "terminal.view",
                  {{"session", From("list", "selected")}})},
            {LayoutSize::Rest(), LayoutSize::Rest(), LayoutSize::Rest()}));
        // A mixer takes `session` as the terminal does, and still gets no link.
        const LayoutEdit mixer =
            SplitPanel(before, "terminal", true, "probe.mixer", Known());
        QVERIFY2(mixer.Refused.isEmpty(), qPrintable(mixer.Refused));
        QCOMPARE(Leaf(mixer.Tree, "probe.mixer-2").Panel,
                 QString("probe.mixer"));
        QVERIFY(Leaf(mixer.Tree, "probe.mixer-2").Links.isEmpty());
        QCOMPARE(Leaf(mixer.Tree, "terminal"), Leaf(before, "terminal"));
        // An unknown type is refused and the tree is as it was.
        const LayoutEdit unknown =
            SplitPanel(before, "terminal", true, "no.such", Known());
        QVERIFY(!unknown.Refused.isEmpty());
        QVERIFY(!unknown.Changed);
        QCOMPARE(unknown.Tree, before);
    }

    void SplitOfAPlaceholderLeafDoesNothingWithoutAType() {
        const Layout before = Tree(Split(
            true, {Leaf("list", "sessions.list"), Leaf("stub", QString())},
            {LayoutSize::Rest(), LayoutSize::Rest()}));
        const LayoutEdit edit = SplitPanel(before, "stub", true, {}, Known());
        QVERIFY(edit.Refused.contains("no panel type"));
        QVERIFY(!edit.Changed);
        QCOMPARE(edit.Tree, before);
        // The type the user chose is the one to split with.
        const LayoutEdit chosen =
            SplitPanel(before, "stub", true, "sessions.list", Known());
        QVERIFY2(chosen.Refused.isEmpty(), qPrintable(chosen.Refused));
        QCOMPARE(Leaf(chosen.Tree, "sessions.list-2").Panel,
                 QString("sessions.list"));
    }

    void SplitGivesAnIdNoLeafHas() {
        const Layout layout = Tree(Split(
            true,
            {Leaf("terminal", "terminal.view"),
             Leaf("terminal-2", "terminal.view"),
             Leaf("terminal-3", "terminal.view")},
            {LayoutSize::Rest(), LayoutSize::Rest(), LayoutSize::Rest()}));
        const LayoutEdit edit =
            SplitPanel(layout, "terminal", true, {}, Known());
        QCOMPARE(Instances(edit.Tree.Root),
                 (QStringList{"terminal", "terminal-4", "terminal-2",
                              "terminal-3"}));
        QVERIFY(
            !SplitPanel(layout, "nobody", true, {}, Known()).Refused.isEmpty());
    }

    void IntoTabsMergesWithTheNextSiblingOrThePreviousOne() {
        const Layout layout = ListAndTerminal();
        const LayoutEdit first = MoveIntoTabs(layout, "list");
        QVERIFY2(first.Refused.isEmpty(), qPrintable(first.Refused));
        // The split had two children: the stack is the whole layout, the
        // moved panel is the shown one.
        QCOMPARE(first.Tree.Root.Kind, ELayoutNodeKind::Tabs);
        QCOMPARE(Instances(first.Tree.Root), (QStringList{"list", "terminal"}));
        QCOMPARE(first.Tree.Root.Active, 0);
        QCOMPARE(first.Focus, QString("list"));
        // The last panel goes with the previous one.
        const LayoutEdit last = MoveIntoTabs(layout, "terminal");
        QCOMPARE(last.Tree.Root.Kind, ELayoutNodeKind::Tabs);
        QCOMPARE(Instances(last.Tree.Root), (QStringList{"list", "terminal"}));
        QCOMPARE(last.Tree.Root.Active, 1);
        // The links are untouched.
        QCOMPARE(Leaf(last.Tree, "terminal").Links,
                 Leaf(layout, "terminal").Links);
        VerifyReadsBack(first.Tree);
        VerifyReadsBack(last.Tree);
    }

    void IntoTabsInAWiderSplitKeepsTheOthersAndJoinsAStack() {
        const Layout layout = Tree(Split(
            true,
            {Leaf("a", "sessions.list"), Leaf("b", "terminal.view"),
             Leaf("c", "terminal.view")},
            {LayoutSize::Px(300), LayoutSize::Px(200), LayoutSize::Rest()}));
        const LayoutEdit ab = MoveIntoTabs(layout, "a");
        QCOMPARE(ab.Tree.Root.Kind, ELayoutNodeKind::Split);
        QCOMPARE(ab.Tree.Root.Children.size(), size_t{2});
        QCOMPARE(Instances(ab.Tree.Root.Children[0]), (QStringList{"a", "b"}));
        QCOMPARE(ab.Tree.Root.Children[0].Active, 0);
        // The stack is as wide as the wider of the two.
        QCOMPARE(ab.Tree.Root.Sizes,
                 (std::vector{LayoutSize::Px(300), LayoutSize::Rest()}));
        // `c` next to the stack joins it, after its tabs, and is shown.
        const LayoutEdit join = MoveIntoTabs(ab.Tree, "c");
        QCOMPARE(join.Tree.Root.Kind, ELayoutNodeKind::Tabs);
        QCOMPARE(Instances(join.Tree.Root), (QStringList{"a", "b", "c"}));
        QCOMPARE(join.Tree.Root.Active, 2);
        // A stack that is the next sibling takes the panel first.
        const LayoutEdit before = MoveIntoTabs(
            Tree(Split(
                true,
                {Leaf("x", "terminal.view"),
                 Tabs({Leaf("y", "terminal.view"), Leaf("z", "terminal.view")},
                      1)},
                {LayoutSize::Rest(), LayoutSize::Rest()})),
            "x");
        QCOMPARE(Instances(before.Tree.Root), (QStringList{"x", "y", "z"}));
        QCOMPARE(before.Tree.Root.Active, 0);
    }

    void IntoTabsIsRefusedWithNothingToMergeWith() {
        const Layout alone = Tree(Leaf("list", "sessions.list"));
        const LayoutEdit one = MoveIntoTabs(alone, "list");
        QVERIFY(!one.Refused.isEmpty());
        QVERIFY(!one.Changed);
        QCOMPARE(one.Tree, alone);

        const Layout stack = Tree(
            Tabs({Leaf("a", "sessions.list"), Leaf("b", "terminal.view")}, 0));
        const LayoutEdit inStack = MoveIntoTabs(stack, "a");
        QVERIFY(inStack.Refused.contains("in tabs already"));
        QCOMPARE(inStack.Tree, stack);
    }

    void OutOfTabsPutsThePanelBackAsASiblingOfTheStack() {
        const Layout layout = Tree(
            Split(true,
                  {Leaf("list", "sessions.list"),
                   Tabs({Leaf("a", "terminal.view"), Leaf("b", "terminal.view"),
                         Leaf("c", "terminal.view")},
                        1)},
                  {LayoutSize::Px(320), LayoutSize::Rest()}));
        const LayoutEdit edit = MoveOutOfTabs(layout, "b");
        QVERIFY2(edit.Refused.isEmpty(), qPrintable(edit.Refused));
        QCOMPARE(edit.Tree.Root.Children.size(), size_t{3});
        QCOMPARE(Instances(edit.Tree.Root.Children[1]),
                 (QStringList{"a", "c"}));
        QCOMPARE(edit.Tree.Root.Children[2].Instance, QString("b"));
        QCOMPARE(edit.Tree.Root.Sizes,
                 (std::vector{LayoutSize::Px(320), LayoutSize::Rest(),
                              LayoutSize::Rest()}));
        // The tab that was shown is gone: the one after it is shown.
        QCOMPARE(edit.Tree.Root.Children[1].Active, 1);
        QCOMPARE(edit.Focus, QString("b"));
        VerifyReadsBack(edit.Tree);

        // A stack of two is no stack once one is out.
        const LayoutEdit two = MoveOutOfTabs(
            Tree(Tabs({Leaf("a", "sessions.list"), Leaf("b", "terminal.view")},
                      0)),
            "a");
        QCOMPARE(two.Tree.Root.Kind, ELayoutNodeKind::Split);
        QCOMPARE(Instances(two.Tree.Root), (QStringList{"b", "a"}));
        QCOMPARE(two.Tree.Root.Children[0].Kind, ELayoutNodeKind::Leaf);
        VerifyReadsBack(two.Tree);
    }

    void OutOfTabsIsRefusedOutsideTabs() {
        const Layout layout = ListAndTerminal();
        const LayoutEdit edit = MoveOutOfTabs(layout, "list");
        QVERIFY(edit.Refused.contains("not in tabs"));
        QVERIFY(!edit.Changed);
        QCOMPARE(edit.Tree, layout);
        QVERIFY(!MoveOutOfTabs(Tree(Leaf("a", "sessions.list")), "a")
                     .Refused.isEmpty());
    }

    void IntoTabsThenOutOfTabsGivesTheSplitBack() {
        const Layout layout = Tree(Split(
            true,
            {Leaf("a", "sessions.list"), Leaf("b", "terminal.view"),
             Leaf("c", "terminal.view")},
            {LayoutSize::Rest(), LayoutSize::Rest(), LayoutSize::Rest()}));
        const LayoutEdit in = MoveIntoTabs(layout, "b");
        const LayoutEdit out = MoveOutOfTabs(in.Tree, "b");
        QVERIFY(out.Refused.isEmpty());
        // `b` went into tabs with `c`; out, it is after the stack, which is
        // `c` alone and so no stack.
        QCOMPARE(Instances(out.Tree.Root), (QStringList{"a", "c", "b"}));
        QCOMPARE(out.Tree.Root.Kind, ELayoutNodeKind::Split);
        QCOMPARE(out.Tree.Root.Children.size(), size_t{3});
    }

    void CloseRemovesThePanelItsLinksAndTheSingleChildSplit() {
        const Layout layout =
            Tree(Split(true,
                       {Leaf("list", "sessions.list"),
                        Split(false,
                              {Leaf("t1", "terminal.view",
                                    {{"session", From("list", "selected")}}),
                               Leaf("t2", "terminal.view",
                                    {{"session", From("list", "selected")}})},
                              {LayoutSize::Rest(), LayoutSize::Rest()})},
                       {LayoutSize::Px(320), LayoutSize::Rest()}));
        const LayoutEdit edit = ClosePanel(layout, "t1");
        QVERIFY2(edit.Refused.isEmpty(), qPrintable(edit.Refused));
        // The inner split has one child left: it is that child.
        QCOMPARE(edit.Tree.Root.Children.size(), size_t{2});
        QCOMPARE(edit.Tree.Root.Children[1].Kind, ELayoutNodeKind::Leaf);
        QCOMPARE(edit.Tree.Root.Children[1].Instance, QString("t2"));
        QCOMPARE(edit.Focus, QString("t2"));
        VerifyReadsBack(edit.Tree);

        // The list goes: the links that came from it go, the terminal that
        // was its neighbour takes the keyboard.
        const LayoutEdit list = ClosePanel(edit.Tree, "list");
        QCOMPARE(list.Tree.Root.Kind, ELayoutNodeKind::Leaf);
        QCOMPARE(list.Tree.Root.Instance, QString("t2"));
        QVERIFY(list.Tree.Root.Links.isEmpty());
        QCOMPARE(list.Focus, QString("t2"));
    }

    void CloseKeepsAChildThatTakesTheRest() {
        const Layout layout = Tree(Split(
            true,
            {Leaf("a", "sessions.list"), Leaf("b", "terminal.view"),
             Leaf("c", "terminal.view")},
            {LayoutSize::Px(200), LayoutSize::Px(300), LayoutSize::Rest()}));
        const LayoutEdit edit = ClosePanel(layout, "c");
        QCOMPARE(Instances(edit.Tree.Root), (QStringList{"a", "b"}));
        QCOMPARE(edit.Tree.Root.Sizes,
                 (std::vector{LayoutSize::Px(200), LayoutSize::Rest()}));
        // Sizes of a split that still has one are not touched.
        const LayoutEdit first = ClosePanel(layout, "a");
        QCOMPARE(first.Tree.Root.Sizes,
                 (std::vector{LayoutSize::Px(300), LayoutSize::Rest()}));
    }

    void CloseInATabStackShowsTheNextTab() {
        const Layout layout = Tree(
            Split(true,
                  {Leaf("list", "sessions.list"),
                   Tabs({Leaf("a", "terminal.view"), Leaf("b", "terminal.view"),
                         Leaf("c", "terminal.view")},
                        1)},
                  {LayoutSize::Px(320), LayoutSize::Rest()}));
        const LayoutEdit edit = ClosePanel(layout, "b");
        QCOMPARE(Instances(edit.Tree.Root.Children[1]),
                 (QStringList{"a", "c"}));
        QCOMPARE(edit.Tree.Root.Children[1].Active, 1);
        QCOMPARE(edit.Focus, QString("c"));
        // Closing a tab before the shown one keeps the shown one shown.
        const LayoutEdit before = ClosePanel(
            Tree(Tabs({Leaf("a", "terminal.view"), Leaf("b", "terminal.view"),
                       Leaf("c", "terminal.view")},
                      2)),
            "a");
        QCOMPARE(before.Tree.Root.Active, 1);
        QCOMPARE(before.Tree.Root
                     .Children[static_cast<size_t>(before.Tree.Root.Active)]
                     .Instance,
                 QString("c"));
    }

    void CloseGivesTheKeyboardToTheNeighbourBeforeIt() {
        const Layout layout = Tree(Split(
            true,
            {Leaf("a", "sessions.list"), Leaf("b", "terminal.view"),
             Leaf("c", "terminal.view")},
            {LayoutSize::Px(200), LayoutSize::Px(200), LayoutSize::Rest()}));
        QCOMPARE(ClosePanel(layout, "b").Focus, QString("a"));
        QCOMPARE(ClosePanel(layout, "c").Focus, QString("b"));
        // The first has none before it: the one after it.
        QCOMPARE(ClosePanel(layout, "a").Focus, QString("b"));
    }

    void CloseOfATabThatIsNotShownGivesTheKeyboardToTheShownOne() {
        const auto stack = [](int active) {
            return Tree(Split(
                true,
                {Leaf("list", "sessions.list"),
                 Tabs({Leaf("a", "terminal.view"), Leaf("b", "terminal.view"),
                       Leaf("c", "terminal.view")},
                      active)},
                {LayoutSize::Px(320), LayoutSize::Rest()}));
        };
        // `a` is shown; `c`, the last, goes: the neighbour by index is `b`,
        // which is a hidden page.
        const LayoutEdit last = ClosePanel(stack(0), "c");
        QCOMPARE(last.Focus, QString("a"));
        QCOMPARE(last.Tree.Root.Children[1].Active, 0);
        // `c` is shown and `a` goes: `c` stays the shown one.
        const LayoutEdit first = ClosePanel(stack(2), "a");
        QCOMPARE(first.Focus, QString("c"));
        // `b` is shown and `a` goes.
        QCOMPARE(ClosePanel(stack(1), "a").Focus, QString("b"));
        // A stack that has one tab left is that tab.
        const LayoutEdit pair = ClosePanel(
            Tree(Split(
                true,
                {Leaf("list", "sessions.list"),
                 Tabs({Leaf("a", "terminal.view"), Leaf("b", "terminal.view")},
                      0)},
                {LayoutSize::Px(320), LayoutSize::Rest()})),
            "b");
        QCOMPARE(pair.Focus, QString("a"));
    }

    void TheShownTabStaysShownWhenAnotherTabLeavesTheStack() {
        // Four tabs, `c` shown: the index of `c` drops by one with `a` gone,
        // and a clamp to the last tab would not save it.
        const auto stack = [] {
            return Tree(Split(
                true,
                {Leaf("list", "sessions.list"),
                 Tabs({Leaf("a", "terminal.view"), Leaf("b", "terminal.view"),
                       Leaf("c", "terminal.view"), Leaf("d", "terminal.view")},
                      2)},
                {LayoutSize::Px(320), LayoutSize::Rest()}));
        };
        const LayoutEdit out = MoveOutOfTabs(stack(), "a");
        QVERIFY2(out.Refused.isEmpty(), qPrintable(out.Refused));
        const LayoutNode& left = out.Tree.Root.Children[1];
        QCOMPARE(Instances(left), (QStringList{"b", "c", "d"}));
        QCOMPARE(left.Active, 1);
        QCOMPARE(left.Children[static_cast<size_t>(left.Active)].Instance,
                 QString("c"));

        const LayoutEdit closed = ClosePanel(stack(), "a");
        const LayoutNode& rest = closed.Tree.Root.Children[1];
        QCOMPARE(Instances(rest), (QStringList{"b", "c", "d"}));
        QCOMPARE(rest.Active, 1);
        QCOMPARE(rest.Children[static_cast<size_t>(rest.Active)].Instance,
                 QString("c"));
    }

    void TheLastPanelCannotBeClosed() {
        const Layout alone = Tree(Leaf("list", "sessions.list"));
        const LayoutEdit edit = ClosePanel(alone, "list");
        QVERIFY(edit.Refused.contains("last panel"));
        QVERIFY(!edit.Changed);
        QCOMPARE(edit.Tree, alone);
        QVERIFY(!ClosePanel(ListAndTerminal(), "nobody").Refused.isEmpty());
    }

    void ALinkIsSetOnlyBetweenPortsOfTheSameType() {
        const Layout layout = Tree(Split(
            true,
            {Leaf("list", "sessions.list"), Leaf("gauge", "probe.gauge"),
             Leaf("meter", "probe.meter"), Leaf("terminal", "terminal.view")},
            {LayoutSize::Rest(), LayoutSize::Rest(), LayoutSize::Rest(),
             LayoutSize::Rest()}));
        const LayoutEdit level =
            SetLink(layout, "meter", "level", "gauge", "level", Known());
        QVERIFY2(level.Refused.isEmpty(), qPrintable(level.Refused));
        QVERIFY(level.Changed);
        QCOMPARE(Leaf(level.Tree, "meter").Links.value("level"),
                 From("gauge", "level"));
        // Another type, no such port, a panel that is not there, itself.
        for (const LayoutEdit& refused : {
                 SetLink(layout, "meter", "level", "gauge", "picked", Known()),
                 SetLink(layout, "terminal", "session", "gauge", "level",
                         Known()),
                 SetLink(layout, "terminal", "nothing", "list", "selected",
                         Known()),
                 SetLink(layout, "terminal", "session", "list", "nothing",
                         Known()),
                 SetLink(layout, "terminal", "session", "nobody", "selected",
                         Known()),
                 SetLink(layout, "gauge", "level", "gauge", "level", Known()),
             }) {
            QVERIFY(!refused.Refused.isEmpty());
            QVERIFY(!refused.Changed);
            QCOMPARE(refused.Tree, layout);
        }
        // The same link again is no change.
        const LayoutEdit again =
            SetLink(level.Tree, "meter", "level", "gauge", "level", Known());
        QVERIFY(again.Refused.isEmpty());
        QVERIFY(!again.Changed);
        // Taking it away.
        const LayoutEdit removed = RemoveLink(level.Tree, "meter", "level");
        QVERIFY(removed.Changed);
        QVERIFY(Leaf(removed.Tree, "meter").Links.isEmpty());
        QCOMPARE(removed.Tree, layout);
        VerifyReadsBack(level.Tree);
    }

    void ALinkThatMakesARingIsRefused() {
        // The mixer feeds the terminal; feeding the mixer from the terminal
        // is impossible (the terminal has no output), so use two mixers.
        const Layout layout = Tree(Split(
            true,
            {Leaf("m1", "probe.mixer"),
             Leaf("m2", "probe.mixer", {{"session", From("m1", "mixed")}})},
            {LayoutSize::Rest(), LayoutSize::Rest()}));
        const LayoutEdit ring =
            SetLink(layout, "m1", "session", "m2", "mixed", Known());
        QVERIFY(ring.Refused.contains("ring"));
        QCOMPARE(ring.Tree, layout);
        // The editor does not offer it.
        const auto choices = LinkChoices(layout, "m1", Known());
        for (const LayoutInputChoices& input : choices) {
            QVERIFY(input.Choices.empty());
        }
    }

    void TheChoicesOfAnInputAreTheOutputsOfItsType() {
        const Layout layout = Tree(
            Split(true,
                  {Leaf("list", "sessions.list"), Leaf("gauge", "probe.gauge"),
                   Leaf("meter", "probe.meter"),
                   Leaf("terminal", "terminal.view",
                        {{"session", From("list", "selected")}})},
                  {LayoutSize::Rest(), LayoutSize::Rest(), LayoutSize::Rest(),
                   LayoutSize::Rest()}));
        const auto terminal = LinkChoices(layout, "terminal", Known());
        QCOMPARE(terminal.size(), size_t{1});
        QCOMPARE(terminal[0].Input, QString("session"));
        QCOMPARE(terminal[0].Type, QString("SessionRef"));
        QCOMPARE(terminal[0].Current.value().From, QString("list"));
        // `gauge.level` is a Level: not an offer for a SessionRef.
        QCOMPARE(terminal[0].Choices,
                 (std::vector<LayoutLinkChoice>{{"list", "selected"},
                                                {"gauge", "picked"}}));
        const auto meter = LinkChoices(layout, "meter", Known());
        QCOMPARE(meter.size(), size_t{1});
        QVERIFY(!meter[0].Current.has_value());
        QCOMPARE(meter[0].Choices,
                 (std::vector<LayoutLinkChoice>{{"gauge", "level"}}));
        // A list has no input; a leaf that is not there has nothing.
        QVERIFY(LinkChoices(layout, "list", Known()).empty());
        QVERIFY(LinkChoices(layout, "nobody", Known()).empty());
    }

    // ---- the window ---------------------------------------------------------

    void EachCommandWritesTheLayoutOnce() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        const QString id = IdIn("list", 1);
        OpenIdIn("list", "terminal", id);

        RunWritesOnce("terminal", "layout.splitRight");
        Layout now = Shell_->CurrentLayout();
        QCOMPARE(Instances(now.Root),
                 (QStringList{"list", "terminal", "terminal-2"}));
        // The new terminal is a panel of the window, fed by nothing: it
        // does not take the session from the source.
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("terminal-2") != nullptr,
                                 TIMEOUT_MS);
        QVERIFY(PanelByInstance("terminal-2")
                    ->property("session")
                    .toMap()
                    .isEmpty());
        // The source keeps its session and shows it.
        QVERIFY(!TerminalOf("terminal")->superseded());
        QCOMPARE(TerminalOf("terminal")->status(), QString("open"));
        QCOMPARE(
            TerminalOf("terminal")->property("session").toMap().value("id"),
            QVariant(id));
        QCOMPARE(SlotCount(), 3);

        RunWritesOnce("terminal-2", "layout.splitDown");
        now = Shell_->CurrentLayout();
        QCOMPARE(Instances(now.Root),
                 (QStringList{"list", "terminal", "terminal-2", "terminal-3"}));
        QCOMPARE(now.Root.Children[2].Kind, ELayoutNodeKind::Split);

        RunWritesOnce("terminal-3", "layout.intoTabs");
        now = Shell_->CurrentLayout();
        QCOMPARE(now.Root.Children[2].Kind, ELayoutNodeKind::Tabs);
        QCOMPARE(now.Root.Children[2].Active, 1);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal-3")->isVisible(),
                                 TIMEOUT_MS);
        QVERIFY(!SlotByInstance("terminal-2")->isVisible());

        RunWritesOnce("terminal-3", "layout.outOfTabs");
        now = Shell_->CurrentLayout();
        QCOMPARE(now.Root.Children[2].Kind, ELayoutNodeKind::Leaf);
        QCOMPARE(Instances(now.Root),
                 (QStringList{"list", "terminal", "terminal-2", "terminal-3"}));
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal-3")->isVisible(),
                                 TIMEOUT_MS);

        RunWritesOnce("terminal-3", "layout.close");
        now = Shell_->CurrentLayout();
        QCOMPARE(Instances(now.Root),
                 (QStringList{"list", "terminal", "terminal-2"}));
        QTRY_COMPARE_WITH_TIMEOUT(SlotCount(), 3, TIMEOUT_MS);
        QVERIFY(PanelByInstance("terminal-3") == nullptr);
        QVERIFY(Problems().isEmpty());
    }

    void TheKeyboardFollowsTheEdit() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        RunWritesOnce("terminal", "layout.splitRight");
        // The source keeps the keyboard through the rebuild of the view.
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal")->hasActiveFocus(),
                                 TIMEOUT_MS);
        // Closing gives it to the neighbour; the commands act on the panel
        // that had the keyboard last when none has it.
        Focus("terminal-2");
        RunWritesOnce("terminal-2", "layout.close");
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal")->hasActiveFocus(),
                                 TIMEOUT_MS);
    }

    void ACommandActsOnTheLastFocusedPanelWhenNoneHasTheKeyboard() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        Focus("terminal");
        // The keyboard leaves the panels, as it does for the palette.
        SlotByInstance("terminal")->setFocus(false);
        QTRY_VERIFY_WITH_TIMEOUT(!SlotByInstance("terminal")->hasActiveFocus(),
                                 TIMEOUT_MS);
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Api_->Commands().Run("layout.splitDown");
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        QCOMPARE(Instances(Shell_->CurrentLayout().Root),
                 (QStringList{"list", "terminal", "terminal-2"}));
        QCOMPARE(Shell_->CurrentLayout().Root.Children[1].Kind,
                 ELayoutNodeKind::Split);
    }

    void SplitWithAnotherTypeTakesTheTypeFromTheArguments() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        RunWritesOnce("terminal", "layout.splitDown",
                      QJsonObject{{"panel", "sessions.list"}});
        const Layout now = Shell_->CurrentLayout();
        QCOMPARE(Leaf(now, "sessions.list-2").Panel, QString("sessions.list"));
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("sessions.list-2") != nullptr,
                                 TIMEOUT_MS);
        QVERIFY(Leaf(now, "sessions.list-2").Links.isEmpty());
    }

    void TheLinkEditorOffersOnlyPortsOfTheTypeOfTheInput() {
        FailOnWarnings();
        WriteProbePlugin();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        QTRY_VERIFY_WITH_TIMEOUT(Loader_->Info("probe.gauge").has_value(),
                                 TIMEOUT_MS);
        Shell_->SetLayout(Tree(
            Split(true,
                  {Leaf("list", "sessions.list"), Leaf("gauge", "probe.gauge"),
                   Leaf("meter", "probe.meter"),
                   Leaf("terminal", "terminal.view",
                        {{"session", From("list", "selected")}})},
                  {LayoutSize::Px(320), LayoutSize::Rest(), LayoutSize::Rest(),
                   LayoutSize::Rest()})));
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("meter") != nullptr &&
                                     PanelByInstance("gauge") != nullptr &&
                                     PanelByInstance("terminal") != nullptr,
                                 TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredLayout(), LayoutToJson(Shell_->CurrentLayout()), TIMEOUT_MS);
        // The event of that write may still be on its way to the window.
        QTest::qWait(300);

        // The terminal: SessionRef outputs, and not the Level of the gauge.
        Focus("terminal");
        Api_->Commands().Run("layout.editLinks");
        QTRY_VERIFY_WITH_TIMEOUT(
            Popup("linkEditor") != nullptr &&
                Popup("linkEditor")->property("visible").toBool(),
            TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            Offered("session"),
            (QStringList{"none", "list.selected", "gauge.picked"}), TIMEOUT_MS);
        // The current link is the chosen entry.
        QCOMPARE(Named("linkInput_session")->property("currentIndex").toInt(),
                 1);

        // Choose another source: one write, and the panel follows the link.
        {
            QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
            Choose("session", "gauge.picked");
            QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
            QTest::qWait(200);
            QCOMPARE(Writes(spy), 1);
        }
        QCOMPARE(Leaf(Shell_->CurrentLayout(), "terminal")
                     .Links.value("session")
                     .From,
                 QString("gauge"));
        QCOMPARE(StoredLayout(), LayoutToJson(Shell_->CurrentLayout()));
        QTRY_COMPARE_WITH_TIMEOUT(
            Named("linkInput_session")->property("currentIndex").toInt(), 2,
            TIMEOUT_MS);
        // The gauge's `picked` is null, so the terminal has no session now.
        QTRY_VERIFY_WITH_TIMEOUT(
            PanelByInstance("terminal")->property("session").isNull() ||
                !PanelByInstance("terminal")->property("session").isValid() ||
                PanelByInstance("terminal")
                    ->property("session")
                    .toMap()
                    .isEmpty(),
            TIMEOUT_MS);

        // The meter: a Level, from the gauge only.
        Focus("meter");
        Api_->Commands().Run("layout.editLinks");
        QTRY_COMPARE_WITH_TIMEOUT(
            Offered("level"), (QStringList{"none", "gauge.level"}), TIMEOUT_MS);
        {
            QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
            Choose("level", "gauge.level");
            QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        }
        QTRY_COMPARE_WITH_TIMEOUT(
            PanelByInstance("meter")->property("level").toInt(), 3, TIMEOUT_MS);
        // "none" takes the link away, again in one write.
        {
            QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
            Choose("level", "none");
            QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
            QTest::qWait(200);
            QCOMPARE(Writes(spy), 1);
        }
        QVERIFY(Leaf(Shell_->CurrentLayout(), "meter").Links.isEmpty());
        QCOMPARE(StoredLayout(), LayoutToJson(Shell_->CurrentLayout()));
        QVERIFY(Problems().isEmpty());
    }

    void ClosingAPanelRemovesTheLinksThatCameFromIt() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        OpenIdIn("list", "terminal", IdIn("list", 0));
        RunWritesOnce("list", "layout.close");
        const Layout now = Shell_->CurrentLayout();
        QCOMPARE(now.Root.Kind, ELayoutNodeKind::Leaf);
        QCOMPARE(now.Root.Instance, QString("terminal"));
        QVERIFY(now.Root.Links.isEmpty());
        // The document has no link to the closed panel either.
        const QJsonObject stored = StoredLayout().value("root").toObject();
        QCOMPARE(stored.value("instance").toString(), QString("terminal"));
        QVERIFY(stored.value("links").toObject().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(SlotCount(), 1, TIMEOUT_MS);
        QVERIFY(PanelByInstance("list") == nullptr);
        // The terminal lost its session and says nothing is linked.
        QTRY_VERIFY_WITH_TIMEOUT(
            PanelByInstance("terminal")->property("session").toMap().isEmpty(),
            TIMEOUT_MS);
        QVERIFY(Problems().isEmpty());
    }

    void EditsOfAnUnwritableLayoutAreAppliedAndSaidNotToBeSaved() {
        FailOnWarnings();
        // A document of a version this roost does not know is not overwritten.
        QJsonObject newer = LayoutToJson(ListAndTerminal());
        newer.insert("version", 99);
        QVERIFY(QDir().mkpath(Dir_->filePath("config")));
        const QString path = Dir_->filePath("config/layout.json");
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write(QJsonDocument(newer).toJson()) > 0);
        }
        const QByteArray stored = ReadFile(path);
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        RunLocal("terminal", "layout.splitRight");
        // The edit is in the window, and the strip says it is not kept.
        QTRY_COMPARE_WITH_TIMEOUT(
            Instances(Shell_->CurrentLayout().Root),
            (QStringList{"list", "terminal", "terminal-2"}), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem("not writable"), TIMEOUT_MS);
        QTest::qWait(300);
        QCOMPARE(ReadFile(path), stored);
    }

    void AFailedWriteIsInTheStripUntilTheNextWriteThatWorks() {
        // No FailOnWarnings: the shell warns of the write that failed.
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        RunWritesOnce("terminal", "layout.splitRight");
        QVERIFY(Problems().isEmpty());

        // roostd cannot write into its config directory.
        const QString config = Dir_->filePath("config");
        const auto open = QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                          QFileDevice::ExeOwner;
        const auto closed = QFileDevice::ReadOwner | QFileDevice::ExeOwner;
        QVERIFY(QFile::setPermissions(config, closed));
        const auto restore = qScopeGuard(
            [&config, open] { QFile::setPermissions(config, open); });
        {
            QFile probe(config + "/probe");
            if (probe.open(QIODevice::WriteOnly)) {
                QSKIP("the directory is writable whatever its mode here");
            }
        }
        Focus("terminal-2");
        Api_->Commands().Run("layout.splitDown");
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem("not saved"), TIMEOUT_MS);
        // The window has the edit.
        QCOMPARE(Instances(Shell_->CurrentLayout().Root),
                 (QStringList{"list", "terminal", "terminal-2", "terminal-3"}));

        // The next write that works clears it.
        QVERIFY(QFile::setPermissions(config, open));
        Focus("terminal-3");
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Api_->Commands().Run("layout.splitRight");
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!HasProblem("not saved"), TIMEOUT_MS);
        QCOMPARE(StoredLayout(), LayoutToJson(Shell_->CurrentLayout()));
    }

    void ThereAreNoLinksToEditOnAPanelWithoutInputs() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        // The command does nothing: no dialog, no problem, no write.
        Focus("list");
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Api_->Commands().Run("layout.editLinks");
        QTest::qWait(300);
        QObject* editor = Popup("linkEditor");
        QVERIFY(editor == nullptr || !editor->property("visible").toBool());
        QVERIFY(Problems().isEmpty());
        QCOMPARE(Writes(spy), 0);

        // The menu item is off for the list and on for the terminal.
        const auto menuOn = [this](const QString& instance) {
            QQuickItem* slot = SlotByInstance(instance);
            QTest::mouseClick(
                Window(), Qt::RightButton, Qt::NoModifier,
                slot->mapToScene(QPointF(slot->width() / 2, 40)).toPoint());
            QObject* popup = Root()->findChild<QObject*>("panelMenu");
            QTRY_VERIFY_WITH_TIMEOUT(
                popup != nullptr && popup->property("visible").toBool() &&
                    popup->property("instance") == instance,
                TIMEOUT_MS);
        };
        menuOn("list");
        QVERIFY(Root()->findChild<QQuickItem*>("menuSplitRight")->isEnabled());
        QVERIFY(!Root()->findChild<QQuickItem*>("menuEditLinks")->isEnabled());
    }

    void TheRightButtonBelongsToATerminalThatTracksTheMouse() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        OpenIdIn("list", "terminal", IdIn("list", 1));
        // The program in the terminal turns on mouse tracking (1000h).
        const auto* pane = TerminalOf("terminal")->findChild<SessionPane*>();
        QVERIFY(pane != nullptr);
        {
            const auto session = pane->Session();
            const auto lock = session->Lock();
            session->Write(lock, "\x1b[?1000h");
        }
        QQuickItem* slot = SlotByInstance("terminal");
        const QPoint at =
            slot->mapToScene(QPointF(slot->width() / 2, slot->height() / 2))
                .toPoint();
        QObject* popup = Root()->findChild<QObject*>("panelMenu");
        QVERIFY(popup != nullptr);

        // The program owns the button: no menu.
        QTest::mouseClick(Window(), Qt::RightButton, Qt::NoModifier, at);
        QTest::qWait(400);
        QVERIFY(!popup->property("visible").toBool());

        // Shift takes it back for roost.
        QTest::mouseClick(Window(), Qt::RightButton, Qt::ShiftModifier, at);
        QTRY_VERIFY_WITH_TIMEOUT(popup->property("visible").toBool(),
                                 TIMEOUT_MS);
        QCOMPARE(popup->property("instance").toString(), QString("terminal"));
    }

    void TheLastPanelStaysAndTheStripSaysWhy() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        RunWritesOnce("list", "layout.close");
        Focus("terminal");
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Api_->Commands().Run("layout.close");
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem("last panel"), TIMEOUT_MS);
        QTest::qWait(200);
        QCOMPARE(Writes(spy), 0);
        QCOMPARE(Shell_->CurrentLayout().Root.Instance, QString("terminal"));
        QCOMPARE(SlotCount(), 1);
        QCOMPARE(StoredLayout(), LayoutToJson(Shell_->CurrentLayout()));
        // The next edit that works clears the reason.
        Api_->Commands().Run("layout.splitRight");
        QTRY_VERIFY_WITH_TIMEOUT(!HasProblem("last panel"), TIMEOUT_MS);
    }

    void ARightClickOnAPanelOpensItsMenu() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        // On the terminal: its session is not open and it does not track the
        // mouse, so the right button is the menu's.
        QQuickItem* slot = SlotByInstance("terminal");
        const QPoint at =
            slot->mapToScene(QPointF(slot->width() / 2, slot->height() / 2))
                .toPoint();
        QTest::mouseClick(Window(), Qt::RightButton, Qt::NoModifier, at);
        QObject* popup = Root()->findChild<QObject*>("panelMenu");
        QVERIFY(popup != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(popup->property("visible").toBool(),
                                 TIMEOUT_MS);
        QCOMPARE(popup->property("instance").toString(), QString("terminal"));
        const auto item = [this](const char* name) {
            return Root()->findChild<QQuickItem*>(name);
        };
        QVERIFY(item("menuSplitRight")->isEnabled());
        QVERIFY(item("menuClose")->isEnabled());
        // In a split, not in tabs: "into tabs" yes, "out of tabs" no.
        QVERIFY(item("menuIntoTabs")->isEnabled());
        QVERIFY(!item("menuOutOfTabs")->isEnabled());
        QVERIFY(item("menuEditLinks")->isEnabled());
        // "Split with…" has the panel types of the plugins.
        QVERIFY(Root()->findChild<QObject*>(
                    "menuSplitWith_sessions.list_right") != nullptr);
        QVERIFY(Root()->findChild<QObject*>(
                    "menuSplitWith_terminal.view_down") != nullptr);

        // The item is clicked in the window: one write.
        QQuickItem* down = item("menuSplitDown");
        const QPoint itemAt =
            down->mapToScene(QPointF(down->width() / 2, down->height() / 2))
                .toPoint();
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        QTest::mouseClick(Window(), Qt::LeftButton, Qt::NoModifier, itemAt);
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        QCOMPARE(Instances(Shell_->CurrentLayout().Root),
                 (QStringList{"list", "terminal", "terminal-2"}));
        QCOMPARE(Shell_->CurrentLayout().Root.Children[1].Kind,
                 ELayoutNodeKind::Split);
    }

    void TheMenuOfTheLastPanelCannotClose() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        RunWritesOnce("list", "layout.close");
        QQuickItem* slot = SlotByInstance("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(slot != nullptr, TIMEOUT_MS);
        QTest::mouseClick(Window(), Qt::RightButton, Qt::NoModifier,
                          slot->mapToScene(QPointF(40, 40)).toPoint());
        QObject* popup = Root()->findChild<QObject*>("panelMenu");
        QTRY_VERIFY_WITH_TIMEOUT(popup->property("visible").toBool(),
                                 TIMEOUT_MS);
        QVERIFY(!Root()->findChild<QQuickItem*>("menuClose")->isEnabled());
        QVERIFY(!Root()->findChild<QQuickItem*>("menuIntoTabs")->isEnabled());
    }

    void AnEditedLayoutSurvivesARestartOfTheWindowAndRoostd() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        RunWritesOnce("terminal", "layout.splitRight");
        RunWritesOnce("terminal-2", "layout.splitDown");
        RunWritesOnce("terminal-3", "layout.intoTabs");
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("terminal-3") != nullptr,
                                 TIMEOUT_MS);
        const Layout before = Shell_->CurrentLayout();
        QCOMPARE(before.Root.Children[2].Kind, ELayoutNodeKind::Tabs);

        CloseWindow();
        StopDaemon();
        QVERIFY(StartDaemon());
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Shell_->CurrentLayout(), before, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(SlotCount(), 4, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("terminal-3") != nullptr &&
                                     PanelByInstance("terminal-2") != nullptr,
                                 TIMEOUT_MS);
        // The tab that was shown is shown, the other one is not.
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal-3")->isVisible(),
                                 TIMEOUT_MS);
        QVERIFY(!SlotByInstance("terminal-2")->isVisible());
        QVERIFY(Problems().isEmpty());
        // And it is still editable: take the panel out of the tabs again.
        RunWritesOnce("terminal-3", "layout.outOfTabs");
        QCOMPARE(Instances(Shell_->CurrentLayout().Root),
                 (QStringList{"list", "terminal", "terminal-2", "terminal-3"}));
        QCOMPARE(Shell_->CurrentLayout().Root.Children[2].Kind,
                 ELayoutNodeKind::Leaf);
    }
};

QTEST_MAIN(LayoutEditTest)
#include "layout_edit_test.moc"
