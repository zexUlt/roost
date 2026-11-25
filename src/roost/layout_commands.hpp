#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

#include "roost/layout_model.hpp"

namespace roost {

// The edits of the layout (design/06-panels.md, "Правка раскладки"): pure
// functions from a tree to a tree, with no QML, no plugins and no store. The
// window applies the result to the layout it shows and writes it once.
//
// Every edit leaves the tree canonical: a split or a stack of tabs with one
// child is replaced by that child, and a split that has no child taking the
// rest gives it to its last one.

struct LayoutEdit {
    // The tree after the edit; the tree as it was when `Refused` is set.
    Layout Tree;
    // Why nothing was done, one sentence for the problem strip; empty when the
    // edit was done.
    QString Refused;
    // The leaf that takes the keyboard after the edit; empty if none.
    QString Focus;
    // False when the edit was done and left the tree as it was (a link set to
    // what it already is): there is nothing to write.
    bool Changed = false;
};

// A leaf of the tree by instance id, or null.
const LayoutNode* FindLeaf(const LayoutNode& node, const QString& instance);

// The leaf that takes the keyboard after an edit of the whole layout (reset,
// undo): `instance` if the tree still has it, else the leaf that shows first.
// Empty for a tree with no leaf.
QString FocusAfterWholeEdit(const Layout& layout, const QString& instance);

// The number of leaves in the tree.
int CountLeaves(const LayoutNode& node);

// An instance id that no leaf of the tree has: "<base>-2", "<base>-3", ...
QString NewInstanceId(const Layout& layout, const QString& base);

// Splits the leaf: the new half, after the source, is a new instance of
// `panel` (the type of the source if empty) with no links: a new panel that
// took the links of the source would take the session from it. `horizontal`:
// side by side, else one under the other. The source panel keeps its instance
// id, so its state and links. Refused for a leaf with no panel type and no
// `panel` to take.
LayoutEdit SplitPanel(const Layout& layout, const QString& instance,
                      bool horizontal, const QString& panel,
                      const LayoutKnownPanels& known);

// Merges the leaf with its next sibling in the split (the previous one if it
// is the last) into a stack of tabs, or adds it to the stack that sibling is;
// the leaf is the shown tab.
LayoutEdit MoveIntoTabs(const Layout& layout, const QString& instance);

// Takes the leaf out of its stack of tabs and puts it next to the stack, as a
// sibling in a split: before the stack if it was the first tab, else after.
LayoutEdit MoveOutOfTabs(const Layout& layout, const QString& instance);

// The way a panel moves in `MovePanel`.
enum class EMoveDirection { Left, Right, Up, Down };

// Moves the leaf one place in the direction. In a split that runs that way
// (left and right: side by side, up and down: stacked) it swaps with the
// neighbour there, a leaf or a whole subtree, and the sizes go with the two
// panels. At the edge of that split, or in a split that runs the other way,
// it leaves the split and goes next to it in the parent split, when that one
// runs the way of the move: before it for left and up, after it for right and
// down, with the rest of the room; a split left with one child is replaced by
// it. Refused for a panel in a stack of tabs, for the only panel, and when no
// split runs that way around the leaf.
LayoutEdit MovePanel(const Layout& layout, const QString& instance,
                     EMoveDirection direction);

// Turns the split that holds the leaf (the nearest one above it, past a stack
// of tabs) from side by side to stacked or back; the children, their order and
// their sizes stay. Refused for the only panel.
LayoutEdit RotateSplit(const Layout& layout, const QString& instance);

// Exchanges the places of two leaves, anywhere in the tree. Each leaf keeps
// its instance id, panel type and links, so its state and what feeds it stay.
// The sizes: two leaves of one split take their sizes along (the list stays
// 320 pixels wide on the other side); in different parents the sizes stay with
// the places, since a width of one split is not the height of another. A leaf
// that was shown (every stack above it showed its branch, the tab ShownTab
// names) is shown at its new place: each stack above it shows its branch, and
// `instance` wins a stack both leaves want. `known` tells the hidden lists.
// Refused for a panel that is not there, for `with` empty or the leaf itself.
LayoutEdit SwapPanels(const Layout& layout, const QString& instance,
                      const QString& with, const LayoutKnownPanels& known);

// Moves the tab of the leaf one place to the left or to the right in its
// stack, over the tabs of hidden lists (they have no button); the shown tab
// stays the same panel. Refused outside a stack and when no tab with a button
// is left on that side.
LayoutEdit MoveTab(const Layout& layout, const QString& instance, bool right,
                   const LayoutKnownPanels& known);

// Removes the leaf. The last leaf of the tree cannot be closed. The links that
// came from it are removed from the leaves they fed. There is no undo.
LayoutEdit ClosePanel(const Layout& layout, const QString& instance);

// Feeds the input of the leaf from the output of another one. Refused when a
// panel or a port is not there, when the types differ, and when the link would
// make a ring of panels that feed one another.
LayoutEdit SetLink(const Layout& layout, const QString& instance,
                   const QString& input, const QString& from,
                   const QString& output, const LayoutKnownPanels& known);

// Removes the link of the input; not a refusal when there is none.
LayoutEdit RemoveLink(const Layout& layout, const QString& instance,
                      const QString& input);

// Replaces the layout with the default one for the panel types of `order` (the
// types in load order of their plugins), DefaultLayoutFor: the leaves of the
// default keep their instance ids, the panels that are not in it go with their
// links. Not a refusal when the layout is the default already (`Changed` is
// false). The window asks the user first.
LayoutEdit ResetLayout(const Layout& layout, const QStringList& order,
                       const LayoutKnownPanels& known);

// The documents of the recent edits, the newest last, for the undo: the window
// keeps them in memory only.
using LayoutHistory = std::deque<Layout>;

// How many edits can be undone.
inline constexpr std::size_t LAYOUT_HISTORY_DEPTH = 20;

// Puts the document as it was before an edit in the history; the oldest goes
// when there are `LAYOUT_HISTORY_DEPTH` of them.
void PushLayoutHistory(LayoutHistory& history, Layout before);

// Takes the newest document out of the history; none when it is empty.
std::optional<Layout> PopLayoutHistory(LayoutHistory& history);

// Hides every list of sessions of the window, or shows them again when they
// are hidden: only the state `ListsHidden` of the layout changes, no node, so
// the panels, their links and their sizes stay. The lists are the panel types
// of `known` that ListPanelTypes names. Hiding is refused when the window has
// no list, and when it has only lists (nothing would be left to show); the
// state that has no list to hide is cleared, not refused. When the keyboard
// is in a list that gets hidden (`instance`), `Focus` is the panel that takes
// it, found as for Close: the neighbour before it in the split (the one after
// it for the first) that shows a panel, else the one next to the split.
LayoutEdit ToggleLists(const Layout& layout, const QString& instance,
                       const LayoutKnownPanels& known);

// The panel types the state of the layout hides now: the lists when
// `ListsHidden`, none otherwise.
QSet<QString> HiddenTypes(const Layout& layout, const LayoutKnownPanels& known);

// Whether nothing of the node is shown: all its leaves are of `hidden` types.
bool IsCollapsed(const LayoutNode& node, const QSet<QString>& hidden);

// The tab of a stack that is shown: `Active`, or, when that one is collapsed,
// the next tab that is not (the previous one if none comes after it).
int ShownTab(const LayoutNode& tabs, const QSet<QString>& hidden);

// An output a link can take from.
struct LayoutLinkChoice {
    QString From;
    QString Output;

    bool operator==(const LayoutLinkChoice&) const = default;
};

// What the link editor shows for one input of a leaf.
struct LayoutInputChoices {
    QString Input;
    QString Type;
    // The link of the document, working or not; none without one.
    std::optional<LayoutLink> Current;
    // The outputs of the other leaves whose declared type is the type of the
    // input, in document order. A choice that would make a ring is left out.
    std::vector<LayoutLinkChoice> Choices;
};

// The inputs of the leaf (by name) with what each can be fed from. Empty for a
// leaf that is not there, has no panel, or has no inputs.
std::vector<LayoutInputChoices> LinkChoices(const Layout& layout,
                                            const QString& instance,
                                            const LayoutKnownPanels& known);

}  // namespace roost
