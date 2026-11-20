#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <vector>

namespace roost {

// The layout of the window (design/06-panels.md, "Раскладка"): one document
// `{version, root}`, a tree of splits, tab stacks and panel leaves. This file
// is the model only: parsing with validation, writing and the default tree.
// No QML and no plugins loaded: the panel types and their ports are an input.

// The ports of a panel type, name -> type string ("SessionRef"), as
// PluginLoader::PanelInfo has them.
struct LayoutPanelPorts {
    QVariantMap Inputs;
    QVariantMap Outputs;
};

// The panel types that can be created, by id ("sessions.list").
using LayoutKnownPanels = QMap<QString, LayoutPanelPorts>;

// The size of a split child along the split: pixels or the rest.
struct LayoutSize {
    // 0 when Fill.
    int Pixels = 0;
    bool Fill = true;

    static LayoutSize Px(int pixels) {
        return {.Pixels = pixels, .Fill = false};
    }
    static LayoutSize Rest() { return {}; }

    bool operator==(const LayoutSize&) const = default;
};

// An input of a leaf fed by the output of another leaf.
struct LayoutLink {
    // The instance id of the source leaf and its output.
    QString From;
    QString Output;
    // Why the link is off; empty for a link that works. Never written: the
    // document keeps the link as it was, so a panel that comes back brings its
    // link back.
    QString Problem;

    bool operator==(const LayoutLink&) const = default;
};

enum class ELayoutNodeKind { Split, Tabs, Leaf };

struct LayoutNode {
    ELayoutNodeKind Kind = ELayoutNodeKind::Leaf;

    // Split: true for "h" (children side by side), false for "v" (stacked).
    bool Horizontal = true;
    // Split and Tabs.
    std::vector<LayoutNode> Children;
    // Split: one per child.
    std::vector<LayoutSize> Sizes;
    // Tabs: the index of the shown child.
    int Active = 0;

    // Leaf: the id of this panel in the window, the panel type, the links by
    // input name.
    QString Instance;
    QString Panel;
    QMap<QString, LayoutLink> Links;
    // Leaf: why there is no panel (an unknown type); empty otherwise. Never
    // written.
    QString Error;

    bool operator==(const LayoutNode&) const = default;
};

struct Layout {
    LayoutNode Root;

    bool operator==(const Layout&) const = default;
};

struct LayoutParseResult {
    // Always usable: the tree of the document with what is wrong repaired or
    // switched off, or the default tree.
    Layout Tree;
    // What was wrong, one line each, with the path of the node.
    QStringList Problems;
    // False when the document has a version this roost does not know: the tree
    // is the default one and the stored document must not be overwritten.
    bool Writable = true;
};

// The version this roost reads and writes.
inline constexpr int LAYOUT_VERSION = 1;

// The layout of the shell before M6: the list on the left (320 pixels), the
// terminal on the right, the `selected` of the list into the `session` of the
// terminal.
Layout DefaultLayout();

// Reads a stored document. Never fails as a whole: see LayoutParseResult.
// `known` decides which leaves are stubs and which links work.
LayoutParseResult ParseLayout(const QJsonValue& document,
                              const LayoutKnownPanels& known);

// The document `{version, root}` of a tree.
QJsonObject LayoutToJson(const Layout& layout);

}  // namespace roost
