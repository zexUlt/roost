#include "roost/layout_commands.hpp"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <utility>

namespace roost {

namespace {

using Path = std::vector<size_t>;

bool PathTo(const LayoutNode& node, const QString& instance, Path& path) {
    if (node.Kind == ELayoutNodeKind::Leaf) {
        return node.Instance == instance;
    }
    for (size_t i = 0; i < node.Children.size(); ++i) {
        path.push_back(i);
        if (PathTo(node.Children[i], instance, path)) {
            return true;
        }
        path.pop_back();
    }
    return false;
}

LayoutNode& At(LayoutNode& root, const Path& path, size_t length) {
    LayoutNode* node = &root;
    for (size_t i = 0; i < length; ++i) {
        node = &node->Children[path[i]];
    }
    return *node;
}

LayoutEdit Refuse(const Layout& layout, QString why) {
    return {.Tree = layout,
            .Refused = std::move(why),
            .Focus = {},
            .Changed = false};
}

LayoutEdit Done(Layout tree, const Layout& before, QString focus) {
    const bool changed = tree != before;
    return {.Tree = std::move(tree),
            .Refused = {},
            .Focus = std::move(focus),
            .Changed = changed};
}

void CollectLeaves(const LayoutNode& node,
                   std::vector<const LayoutNode*>& leaves) {
    if (node.Kind == ELayoutNodeKind::Leaf) {
        leaves.push_back(&node);
        return;
    }
    for (const LayoutNode& child : node.Children) {
        CollectLeaves(child, leaves);
    }
}

void EraseLinksFrom(LayoutNode& node, const QString& instance) {
    if (node.Kind == ELayoutNodeKind::Leaf) {
        for (auto it = node.Links.begin(); it != node.Links.end();) {
            it = it->From == instance ? node.Links.erase(it) : std::next(it);
        }
        return;
    }
    for (LayoutNode& child : node.Children) {
        EraseLinksFrom(child, instance);
    }
}

// The leaf that shows when the node is looked at: the first one of a split,
// the shown tab of a stack.
const LayoutNode& ShownLeaf(const LayoutNode& node) {
    const LayoutNode* at = &node;
    while (at->Kind != ELayoutNodeKind::Leaf) {
        const size_t index =
            at->Kind == ELayoutNodeKind::Tabs
                ? std::min(static_cast<size_t>(std::max(at->Active, 0)),
                           at->Children.size() - 1)
                : 0;
        at = &at->Children[index];
    }
    return *at;
}

// Brings the node at `path` back to a canonical tree after its children
// changed, and the nodes above it. A split whose children all have a size
// gives the rest to the last one, and a node with one child is that child.
void Tidy(LayoutNode& root, const Path& path) {
    for (size_t length = path.size() + 1; length-- > 0;) {
        LayoutNode& node = At(root, path, length);
        if (node.Kind == ELayoutNodeKind::Leaf || node.Children.empty()) {
            continue;
        }
        if (node.Kind == ELayoutNodeKind::Tabs) {
            node.Active = std::clamp(
                node.Active, 0, static_cast<int>(node.Children.size()) - 1);
        } else if (length == path.size() &&
                   std::none_of(
                       node.Sizes.begin(), node.Sizes.end(),
                       [](const LayoutSize& size) { return size.Fill; })) {
            node.Sizes.back() = LayoutSize::Rest();
        }
        if (node.Children.size() == 1) {
            LayoutNode only = std::move(node.Children.front());
            node = std::move(only);
        }
    }
}

// Whether `start` feeds, through links, the panel `target`: the panel `start`
// itself, or any panel it takes an input from, and so on.
bool DependsOn(const LayoutNode& root, const QString& start,
               const QString& target) {
    std::vector<const LayoutNode*> leaves;
    CollectLeaves(root, leaves);
    QSet<QString> seen;
    QStringList todo{start};
    while (!todo.isEmpty()) {
        const QString at = todo.takeLast();
        if (at == target) {
            return true;
        }
        if (seen.contains(at)) {
            continue;
        }
        seen.insert(at);
        for (const LayoutNode* leaf : leaves) {
            if (leaf->Instance != at) {
                continue;
            }
            for (const LayoutLink& link : leaf->Links) {
                todo.append(link.From);
            }
        }
    }
    return false;
}

// The type of the output `output` of the panel `panel`, or empty.
QString OutputType(const LayoutKnownPanels& known, const QString& panel,
                   const QString& output) {
    const auto found = known.constFind(panel);
    return found == known.constEnd() ? QString()
                                     : found->Outputs.value(output).toString();
}

QString InputType(const LayoutKnownPanels& known, const QString& panel,
                  const QString& input) {
    const auto found = known.constFind(panel);
    return found == known.constEnd() ? QString()
                                     : found->Inputs.value(input).toString();
}

// The leaf that is shown when the node is looked at, the hidden ones left
// out; null when the node is collapsed.
const LayoutNode* ShownVisibleLeaf(const LayoutNode& node,
                                   const QSet<QString>& hidden) {
    if (node.Kind == ELayoutNodeKind::Leaf) {
        return hidden.contains(node.Panel) ? nullptr : &node;
    }
    if (node.Children.empty()) {
        return nullptr;
    }
    if (node.Kind == ELayoutNodeKind::Tabs) {
        return ShownVisibleLeaf(
            node.Children[static_cast<size_t>(ShownTab(node, hidden))], hidden);
    }
    for (const LayoutNode& child : node.Children) {
        if (const LayoutNode* found = ShownVisibleLeaf(child, hidden)) {
            return found;
        }
    }
    return nullptr;
}

// The panel that takes the keyboard from `instance` once `hidden` types are
// collapsed: the neighbours in each split above it, nearest first and the one
// before it first, then the shown tab of each stack above it.
QString FocusAfterHide(const LayoutNode& root, const QString& instance,
                       const QSet<QString>& hidden) {
    Path path;
    if (!PathTo(root, instance, path)) {
        return {};
    }
    for (size_t depth = path.size(); depth-- > 0;) {
        const LayoutNode& parent = [&]() -> const LayoutNode& {
            const LayoutNode* node = &root;
            for (size_t i = 0; i < depth; ++i) {
                node = &node->Children[path[i]];
            }
            return *node;
        }();
        const auto own = static_cast<std::ptrdiff_t>(path[depth]);
        std::vector<std::ptrdiff_t> order;
        for (std::ptrdiff_t at = 0;
             at < static_cast<std::ptrdiff_t>(parent.Children.size()); ++at) {
            if (at != own) {
                order.push_back(at);
            }
        }
        std::ranges::stable_sort(order, [own](auto a, auto b) {
            const auto da = a > own ? a - own : own - a;
            const auto db = b > own ? b - own : own - b;
            return da != db ? da < db : a < b;
        });
        if (parent.Kind == ELayoutNodeKind::Tabs) {
            order = {ShownTab(parent, hidden)};
        }
        for (const std::ptrdiff_t at : order) {
            if (const LayoutNode* leaf = ShownVisibleLeaf(
                    parent.Children[static_cast<size_t>(at)], hidden)) {
                return leaf->Instance;
            }
        }
    }
    return {};
}

// Whether the leaf at `path` is looked at: every stack above it shows the
// branch it is in.
bool IsShownAt(const LayoutNode& root, const Path& path,
               const QSet<QString>& hidden) {
    const LayoutNode* node = &root;
    for (const size_t index : path) {
        if (node->Kind == ELayoutNodeKind::Tabs &&
            static_cast<size_t>(ShownTab(*node, hidden)) != index) {
            return false;
        }
        node = &node->Children[index];
    }
    return true;
}

// Makes every stack above the leaf at `path` show the branch it is in.
void ShowAlong(LayoutNode& root, const Path& path) {
    LayoutNode* node = &root;
    for (const size_t index : path) {
        if (node->Kind == ELayoutNodeKind::Tabs) {
            node->Active = static_cast<int>(index);
        }
        node = &node->Children[index];
    }
}

}  // namespace

bool IsCollapsed(const LayoutNode& node, const QSet<QString>& hidden) {
    if (hidden.isEmpty()) {
        return false;
    }
    if (node.Kind == ELayoutNodeKind::Leaf) {
        return hidden.contains(node.Panel);
    }
    return std::ranges::all_of(node.Children, [&hidden](const LayoutNode& c) {
        return IsCollapsed(c, hidden);
    });
}

int ShownTab(const LayoutNode& tabs, const QSet<QString>& hidden) {
    const int count = static_cast<int>(tabs.Children.size());
    const int active = std::clamp(tabs.Active, 0, std::max(count - 1, 0));
    if (count == 0 ||
        !IsCollapsed(tabs.Children[static_cast<size_t>(active)], hidden)) {
        return active;
    }
    for (int i = active + 1; i < count; ++i) {
        if (!IsCollapsed(tabs.Children[static_cast<size_t>(i)], hidden)) {
            return i;
        }
    }
    for (int i = active - 1; i >= 0; --i) {
        if (!IsCollapsed(tabs.Children[static_cast<size_t>(i)], hidden)) {
            return i;
        }
    }
    return active;
}

QSet<QString> HiddenTypes(const Layout& layout,
                          const LayoutKnownPanels& known) {
    if (!layout.ListsHidden) {
        return {};
    }
    const QStringList lists = ListPanelTypes(known.keys(), known);
    return {lists.begin(), lists.end()};
}

LayoutEdit ToggleLists(const Layout& layout, const QString& instance,
                       const LayoutKnownPanels& known) {
    const QStringList listTypes = ListPanelTypes(known.keys(), known);
    const QSet<QString> lists(listTypes.begin(), listTypes.end());
    std::vector<const LayoutNode*> leaves;
    CollectLeaves(layout.Root, leaves);
    const auto isList = [&lists](const LayoutNode* leaf) {
        return lists.contains(leaf->Panel);
    };
    const bool anyList = std::ranges::any_of(leaves, isList);
    Layout tree = layout;
    if (layout.ListsHidden || !anyList) {
        // Showing: the state goes. With no list to show it only goes.
        if (!layout.ListsHidden) {
            return Refuse(layout, QStringLiteral("there is no list of sessions "
                                                 "to hide"));
        }
        tree.ListsHidden = false;
        return Done(
            std::move(tree), layout,
            FindLeaf(layout.Root, instance) != nullptr ? instance : QString());
    }
    if (std::ranges::all_of(leaves, isList)) {
        return Refuse(layout, QStringLiteral("the window has only lists: "
                                             "hiding them would leave "
                                             "nothing to show"));
    }
    tree.ListsHidden = true;
    // The edit rebuilds the view, and the keyboard stays where it was unless
    // that panel goes.
    const LayoutNode* own = FindLeaf(tree.Root, instance);
    QString focus = own != nullptr ? instance : QString();
    if (own != nullptr && lists.contains(own->Panel)) {
        focus = FocusAfterHide(tree.Root, instance, lists);
    }
    return Done(std::move(tree), layout, std::move(focus));
}

const LayoutNode* FindLeaf(const LayoutNode& node, const QString& instance) {
    if (node.Kind == ELayoutNodeKind::Leaf) {
        return node.Instance == instance ? &node : nullptr;
    }
    for (const LayoutNode& child : node.Children) {
        if (const LayoutNode* found = FindLeaf(child, instance)) {
            return found;
        }
    }
    return nullptr;
}

int CountLeaves(const LayoutNode& node) {
    std::vector<const LayoutNode*> leaves;
    CollectLeaves(node, leaves);
    return static_cast<int>(leaves.size());
}

QString NewInstanceId(const Layout& layout, const QString& base) {
    QString name;
    for (int n = 2; name.isEmpty() || FindLeaf(layout.Root, name) != nullptr;
         ++n) {
        name = QStringLiteral("%1-%2").arg(base).arg(n);
    }
    return name;
}

LayoutEdit SplitPanel(const Layout& layout, const QString& instance,
                      bool horizontal, const QString& panel,
                      const LayoutKnownPanels& known) {
    Path path;
    if (!PathTo(layout.Root, instance, path)) {
        return Refuse(
            layout,
            QStringLiteral("there is no panel %1 to split").arg(instance));
    }
    Layout tree = layout;
    LayoutNode& source = At(tree.Root, path, path.size());
    const QString type = panel.isEmpty() ? source.Panel : panel;
    if (type.isEmpty()) {
        // A placeholder leaf has no type to make another one of.
        return Refuse(
            layout,
            QStringLiteral("%1 has no panel type to split with").arg(instance));
    }
    if (!panel.isEmpty() && panel != source.Panel && !known.contains(panel)) {
        return Refuse(layout,
                      QStringLiteral("there is no panel type %1").arg(panel));
    }

    LayoutNode added;
    added.Kind = ELayoutNodeKind::Leaf;
    added.Panel = type;
    // "terminal-2" splits into "terminal-3", not "terminal-2-2".
    static const QRegularExpression COUNTER_SUFFIX(QStringLiteral("-[0-9]+$"));
    QString base = type == source.Panel ? source.Instance : type;
    base.remove(COUNTER_SUFFIX);
    added.Instance = NewInstanceId(layout, base);
    if (!path.empty()) {
        LayoutNode& parent = At(tree.Root, path, path.size() - 1);
        if (parent.Kind == ELayoutNodeKind::Split &&
            parent.Horizontal == horizontal) {
            const auto at = static_cast<std::ptrdiff_t>(path.back()) + 1;
            parent.Children.insert(parent.Children.begin() + at,
                                   std::move(added));
            parent.Sizes.insert(parent.Sizes.begin() + at, LayoutSize::Rest());
            return Done(std::move(tree), layout, instance);
        }
    }
    LayoutNode split;
    split.Kind = ELayoutNodeKind::Split;
    split.Horizontal = horizontal;
    split.Children.push_back(std::move(source));
    split.Children.push_back(std::move(added));
    split.Sizes = {LayoutSize::Rest(), LayoutSize::Rest()};
    source = std::move(split);
    return Done(std::move(tree), layout, instance);
}

LayoutEdit MoveIntoTabs(const Layout& layout, const QString& instance) {
    Path path;
    if (!PathTo(layout.Root, instance, path)) {
        return Refuse(layout,
                      QStringLiteral("there is no panel %1").arg(instance));
    }
    if (path.empty()) {
        return Refuse(layout, QStringLiteral("%1 is the only panel: there is "
                                             "nothing to put in tabs with")
                                  .arg(instance));
    }
    Layout tree = layout;
    const Path parentPath(path.begin(), path.end() - 1);
    LayoutNode& parent = At(tree.Root, path, path.size() - 1);
    if (parent.Kind != ELayoutNodeKind::Split || parent.Children.size() < 2) {
        return Refuse(layout,
                      QStringLiteral("%1 is in tabs already").arg(instance));
    }
    const size_t own = path.back();
    const size_t other = own + 1 < parent.Children.size() ? own + 1 : own - 1;
    const bool otherAfter = other > own;

    LayoutNode merged;
    if (parent.Children[other].Kind == ELayoutNodeKind::Tabs) {
        merged = std::move(parent.Children[other]);
        const auto at =
            otherAfter ? merged.Children.begin() : merged.Children.end();
        const auto index = at - merged.Children.begin();
        merged.Children.insert(at, std::move(parent.Children[own]));
        merged.Active = static_cast<int>(index);
    } else {
        merged.Kind = ELayoutNodeKind::Tabs;
        if (otherAfter) {
            merged.Children.push_back(std::move(parent.Children[own]));
            merged.Children.push_back(std::move(parent.Children[other]));
            merged.Active = 0;
        } else {
            merged.Children.push_back(std::move(parent.Children[other]));
            merged.Children.push_back(std::move(parent.Children[own]));
            merged.Active = 1;
        }
    }
    // The stack takes the rest if either of the two did; else the wider size.
    const LayoutSize a = parent.Sizes[own];
    const LayoutSize b = parent.Sizes[other];
    const LayoutSize size = a.Fill || b.Fill
                                ? LayoutSize::Rest()
                                : LayoutSize::Px(std::max(a.Pixels, b.Pixels));

    const size_t first = std::min(own, other);
    const size_t second = std::max(own, other);
    parent.Children.erase(parent.Children.begin() +
                          static_cast<std::ptrdiff_t>(second));
    parent.Sizes.erase(parent.Sizes.begin() +
                       static_cast<std::ptrdiff_t>(second));
    parent.Children[first] = std::move(merged);
    parent.Sizes[first] = size;
    Tidy(tree.Root, parentPath);
    return Done(std::move(tree), layout, instance);
}

LayoutEdit MoveOutOfTabs(const Layout& layout, const QString& instance) {
    Path path;
    if (!PathTo(layout.Root, instance, path)) {
        return Refuse(layout,
                      QStringLiteral("there is no panel %1").arg(instance));
    }
    Layout tree = layout;
    if (path.empty() ||
        At(tree.Root, path, path.size() - 1).Kind != ELayoutNodeKind::Tabs) {
        return Refuse(layout,
                      QStringLiteral("%1 is not in tabs").arg(instance));
    }
    const Path stackPath(path.begin(), path.end() - 1);
    LayoutNode& stack = At(tree.Root, stackPath, stackPath.size());
    const auto index = static_cast<std::ptrdiff_t>(path.back());
    // The first tab goes before the stack, the others after it: out of tabs
    // undoes into tabs, which merged the panel with its next sibling (the
    // previous one if it was the last).
    const bool before = index == 0;
    LayoutNode leaf = std::move(stack.Children[path.back()]);
    stack.Children.erase(stack.Children.begin() + index);
    if (stack.Active > index) {
        --stack.Active;
    }
    if (stack.Children.empty()) {
        // A stack of one tab, as a stored document may have: the tab is all
        // that is left to move.
        stack = std::move(leaf);
        return Done(std::move(tree), layout, instance);
    }
    Tidy(tree.Root, stackPath);

    LayoutNode& left = At(tree.Root, stackPath, stackPath.size());
    if (!stackPath.empty()) {
        LayoutNode& parent = At(tree.Root, stackPath, stackPath.size() - 1);
        if (parent.Kind == ELayoutNodeKind::Split) {
            const auto at = static_cast<std::ptrdiff_t>(stackPath.back()) +
                            (before ? 0 : 1);
            parent.Children.insert(parent.Children.begin() + at,
                                   std::move(leaf));
            parent.Sizes.insert(parent.Sizes.begin() + at, LayoutSize::Rest());
            return Done(std::move(tree), layout, instance);
        }
    }
    LayoutNode split;
    split.Kind = ELayoutNodeKind::Split;
    split.Horizontal = true;
    split.Children.push_back(std::move(before ? leaf : left));
    split.Children.push_back(std::move(before ? left : leaf));
    split.Sizes = {LayoutSize::Rest(), LayoutSize::Rest()};
    left = std::move(split);
    return Done(std::move(tree), layout, instance);
}

LayoutEdit SwapPanels(const Layout& layout, const QString& instance,
                      const QString& with, const LayoutKnownPanels& known) {
    Path path;
    if (!PathTo(layout.Root, instance, path)) {
        return Refuse(
            layout,
            QStringLiteral("there is no panel %1 to swap").arg(instance));
    }
    if (with.isEmpty()) {
        return Refuse(
            layout,
            QStringLiteral("choose the panel to swap %1 with: use \"Swap "
                           "with…\" in the panel menu")
                .arg(instance));
    }
    if (with == instance) {
        return Refuse(
            layout,
            QStringLiteral("%1 cannot be swapped with itself").arg(instance));
    }
    Path otherPath;
    if (!PathTo(layout.Root, with, otherPath)) {
        return Refuse(
            layout,
            QStringLiteral("there is no panel %1 to swap with").arg(with));
    }
    Layout tree = layout;
    const QSet<QString> hidden = HiddenTypes(layout, known);
    // Whether each leaf is looked at before the swap: every stack above it
    // shows the branch it is in (the tab ShownTab names).
    const bool ownShown = IsShownAt(layout.Root, path, hidden);
    const bool otherShown = IsShownAt(layout.Root, otherPath, hidden);

    // A leaf is never an ancestor of another, so both parents exist.
    LayoutNode& parent = At(tree.Root, path, path.size() - 1);
    LayoutNode& otherParent = At(tree.Root, otherPath, otherPath.size() - 1);
    const size_t own = path.back();
    const size_t other = otherPath.back();
    std::swap(parent.Children[own], otherParent.Children[other]);
    if (&parent == &otherParent && parent.Kind == ELayoutNodeKind::Split) {
        std::swap(parent.Sizes[own], parent.Sizes[other]);
    }
    // A leaf that was shown is shown at its new place: each stack above it
    // shows its branch. The panel the command ran on goes last, so it wins a
    // stack both want.
    if (otherShown) {
        ShowAlong(tree.Root, path);
    }
    if (ownShown) {
        ShowAlong(tree.Root, otherPath);
    }
    return Done(std::move(tree), layout, instance);
}

LayoutEdit MoveTab(const Layout& layout, const QString& instance, bool right,
                   const LayoutKnownPanels& known) {
    Path path;
    if (!PathTo(layout.Root, instance, path)) {
        return Refuse(
            layout,
            QStringLiteral("there is no panel %1 to move").arg(instance));
    }
    Layout tree = layout;
    if (path.empty() ||
        At(tree.Root, path, path.size() - 1).Kind != ELayoutNodeKind::Tabs) {
        return Refuse(layout,
                      QStringLiteral("%1 is not in tabs").arg(instance));
    }
    LayoutNode& stack = At(tree.Root, path, path.size() - 1);
    const size_t index = path.back();
    // The tabs that have a button: the ones of the hidden lists have none, so
    // the move steps over them (the menu agrees: LayoutView.qml describe()).
    const QSet<QString> hidden = HiddenTypes(layout, known);
    const auto count = static_cast<std::ptrdiff_t>(stack.Children.size());
    const std::ptrdiff_t step = right ? 1 : -1;
    std::ptrdiff_t at = static_cast<std::ptrdiff_t>(index) + step;
    while (at >= 0 && at < count &&
           IsCollapsed(stack.Children[static_cast<size_t>(at)], hidden)) {
        at += step;
    }
    if (at < 0 || at >= count) {
        return Refuse(layout, QStringLiteral("%1 is already the %2 tab")
                                  .arg(instance, right ? "last" : "first"));
    }
    const auto next = static_cast<size_t>(at);
    std::swap(stack.Children[index], stack.Children[next]);
    // The shown tab is the same panel after the move.
    if (static_cast<size_t>(stack.Active) == index) {
        stack.Active = static_cast<int>(next);
    } else if (static_cast<size_t>(stack.Active) == next) {
        stack.Active = static_cast<int>(index);
    }
    return Done(std::move(tree), layout, instance);
}

LayoutEdit ClosePanel(const Layout& layout, const QString& instance) {
    Path path;
    if (!PathTo(layout.Root, instance, path)) {
        return Refuse(
            layout,
            QStringLiteral("there is no panel %1 to close").arg(instance));
    }
    if (path.empty() || CountLeaves(layout.Root) < 2) {
        return Refuse(layout, QStringLiteral("%1 is the last panel and cannot "
                                             "be closed")
                                  .arg(instance));
    }
    Layout tree = layout;
    const Path parentPath(path.begin(), path.end() - 1);
    LayoutNode& parent = At(tree.Root, path, path.size() - 1);
    const size_t index = path.back();
    // The keyboard goes to the neighbour before the closed panel in a split
    // (the one after it for the first), and in a stack to the tab that is
    // shown once the panel is gone.
    const bool stack = parent.Kind == ELayoutNodeKind::Tabs;
    QString focus;
    if (!stack && parent.Children.size() > 1) {
        const size_t neighbour = index > 0 ? index - 1 : index + 1;
        focus = ShownLeaf(parent.Children[neighbour]).Instance;
    }

    parent.Children.erase(parent.Children.begin() +
                          static_cast<std::ptrdiff_t>(index));
    if (parent.Kind == ELayoutNodeKind::Split) {
        parent.Sizes.erase(parent.Sizes.begin() +
                           static_cast<std::ptrdiff_t>(index));
    } else if (parent.Active > static_cast<int>(index)) {
        --parent.Active;
    }
    EraseLinksFrom(tree.Root, instance);
    Tidy(tree.Root, parentPath);
    const LayoutNode& left = At(tree.Root, parentPath, parentPath.size());
    if (stack &&
        (left.Kind == ELayoutNodeKind::Leaf || !left.Children.empty())) {
        focus = ShownLeaf(left).Instance;
    }
    return Done(std::move(tree), layout, focus);
}

LayoutEdit SetLink(const Layout& layout, const QString& instance,
                   const QString& input, const QString& from,
                   const QString& output, const LayoutKnownPanels& known) {
    const LayoutNode* leaf = FindLeaf(layout.Root, instance);
    if (leaf == nullptr) {
        return Refuse(layout,
                      QStringLiteral("there is no panel %1").arg(instance));
    }
    const QString inType = InputType(known, leaf->Panel, input);
    if (inType.isEmpty()) {
        return Refuse(
            layout,
            QStringLiteral("%1 has no input %2").arg(leaf->Panel, input));
    }
    if (from == instance) {
        return Refuse(layout, QStringLiteral("a panel cannot feed itself"));
    }
    const LayoutNode* source = FindLeaf(layout.Root, from);
    if (source == nullptr) {
        return Refuse(layout, QStringLiteral("there is no panel %1").arg(from));
    }
    const QString outType = OutputType(known, source->Panel, output);
    if (outType.isEmpty()) {
        return Refuse(
            layout,
            QStringLiteral("%1 has no output %2").arg(source->Panel, output));
    }
    if (outType != inType) {
        return Refuse(layout,
                      QStringLiteral("the output %1 is %2, the input %3 is %4")
                          .arg(output, outType, input, inType));
    }
    if (DependsOn(layout.Root, from, instance)) {
        return Refuse(layout, QStringLiteral("%1 already takes its input from "
                                             "%2: the link would make a ring")
                                  .arg(from, instance));
    }
    Layout tree = layout;
    // The leaf is found again in the copy.
    Path path;
    PathTo(tree.Root, instance, path);
    At(tree.Root, path, path.size())
        .Links.insert(input, {.From = from, .Output = output, .Problem = {}});
    // The problem of the link is the parser's to say; a link that was off for
    // another reason and is set to the same ends is not a change.
    if (const auto old = leaf->Links.constFind(input);
        old != leaf->Links.constEnd() && old->From == from &&
        old->Output == output) {
        return Done(layout, layout, instance);
    }
    return Done(std::move(tree), layout, instance);
}

LayoutEdit RemoveLink(const Layout& layout, const QString& instance,
                      const QString& input) {
    Path path;
    if (!PathTo(layout.Root, instance, path)) {
        return Refuse(layout,
                      QStringLiteral("there is no panel %1").arg(instance));
    }
    Layout tree = layout;
    At(tree.Root, path, path.size()).Links.remove(input);
    return Done(std::move(tree), layout, instance);
}

LayoutEdit ResetLayout(const Layout& layout, const QStringList& order,
                       const LayoutKnownPanels& known) {
    return Done(DefaultLayoutFor(order, known).Tree, layout, {});
}

void PushLayoutHistory(LayoutHistory& history, Layout before) {
    history.push_back(std::move(before));
    while (history.size() > LAYOUT_HISTORY_DEPTH) {
        history.pop_front();
    }
}

std::optional<Layout> PopLayoutHistory(LayoutHistory& history) {
    if (history.empty()) {
        return std::nullopt;
    }
    Layout last = std::move(history.back());
    history.pop_back();
    return last;
}

std::vector<LayoutInputChoices> LinkChoices(const Layout& layout,
                                            const QString& instance,
                                            const LayoutKnownPanels& known) {
    std::vector<LayoutInputChoices> inputs;
    const LayoutNode* leaf = FindLeaf(layout.Root, instance);
    if (leaf == nullptr) {
        return inputs;
    }
    const auto own = known.constFind(leaf->Panel);
    if (own == known.constEnd()) {
        return inputs;
    }
    std::vector<const LayoutNode*> leaves;
    CollectLeaves(layout.Root, leaves);
    for (auto it = own->Inputs.constBegin(); it != own->Inputs.constEnd();
         ++it) {
        LayoutInputChoices choices;
        choices.Input = it.key();
        choices.Type = it.value().toString();
        if (const auto link = leaf->Links.constFind(it.key());
            link != leaf->Links.constEnd()) {
            choices.Current = link.value();
        }
        for (const LayoutNode* other : leaves) {
            if (other == leaf ||
                DependsOn(layout.Root, other->Instance, instance)) {
                continue;
            }
            const auto source = known.constFind(other->Panel);
            if (source == known.constEnd()) {
                continue;
            }
            for (auto out = source->Outputs.constBegin();
                 out != source->Outputs.constEnd(); ++out) {
                if (out.value().toString() == choices.Type) {
                    choices.Choices.push_back(
                        {.From = other->Instance, .Output = out.key()});
                }
            }
        }
        inputs.push_back(std::move(choices));
    }
    return inputs;
}

}  // namespace roost
