#include "roost/layout_model.hpp"

#include <QJsonArray>
#include <QSet>

#include <cmath>
#include <optional>
#include <utility>

namespace roost {

namespace {

constexpr auto FILL = "fill";

// A problem is a line with the path of the node: "root.children[1]: ...".
class Problems {
public:
    void Add(const QString& path, const QString& text) {
        Lines_.append(QStringLiteral("%1: %2").arg(path, text));
    }
    QStringList Take() { return std::move(Lines_); }

private:
    QStringList Lines_;
};

bool IsWholeNumber(const QJsonValue& value) {
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    return std::floor(number) == number && std::abs(number) < 1e9;
}

// The window of before M6 between two panel types: `input` of the terminal
// takes the `selected` of the list.
LayoutNode DefaultRoot(const QString& listPanel, const QString& terminalPanel,
                       const QString& terminalInput) {
    LayoutNode list;
    list.Instance = QStringLiteral("list");
    list.Panel = listPanel;

    LayoutNode terminal;
    terminal.Instance = QStringLiteral("terminal");
    terminal.Panel = terminalPanel;
    terminal.Links.insert(terminalInput, {.From = QStringLiteral("list"),
                                          .Output = QStringLiteral("selected"),
                                          .Problem = {}});

    LayoutNode root;
    root.Kind = ELayoutNodeKind::Split;
    root.Horizontal = true;
    root.Children = {std::move(list), std::move(terminal)};
    root.Sizes = {LayoutSize::Px(320), LayoutSize::Rest()};
    return root;
}

// The names of the entries of `declared` of the type.
QStringList NamesOfType(const QVariantMap& declared, const QString& type) {
    QStringList names;
    for (auto it = declared.begin(); it != declared.end(); ++it) {
        if (it.value().toString() == type) {
            names.append(it.key());
        }
    }
    return names;
}

// Parsing: the structure only. Instances, panels and links are checked after
// the whole tree is read (Resolve), because a link may point forward.

std::optional<LayoutNode> ParseNode(const QJsonValue& value,
                                    const QString& path, Problems& problems);

LayoutSize ParseSize(const QJsonValue& value, const QString& path,
                     Problems& problems) {
    if (value.isString() && value.toString() == QLatin1String(FILL)) {
        return LayoutSize::Rest();
    }
    if (IsWholeNumber(value) && value.toDouble() > 0) {
        return LayoutSize::Px(static_cast<int>(value.toDouble()));
    }
    problems.Add(path,
                 QStringLiteral("a size is a positive number of pixels or "
                                "\"fill\"; \"fill\" is used"));
    return LayoutSize::Rest();
}

// The children of a split or a stack that could be read, and for a split their
// sizes, kept in step: a dropped child takes its size with it.
std::vector<LayoutNode> ParseChildren(const QJsonValue& value,
                                      const QString& path, Problems& problems,
                                      std::vector<int>& kept) {
    std::vector<LayoutNode> children;
    if (!value.isArray()) {
        problems.Add(path, QStringLiteral("must be an array; the node is "
                                          "dropped"));
        return children;
    }
    const QJsonArray array = value.toArray();
    for (int i = 0; i < array.size(); ++i) {
        std::optional<LayoutNode> child = ParseNode(
            array.at(i), QStringLiteral("%1[%2]").arg(path).arg(i), problems);
        if (child) {
            children.push_back(std::move(*child));
            kept.push_back(i);
        }
    }
    return children;
}

std::optional<LayoutNode> ParseSplit(const QJsonObject& object,
                                     const QString& path, Problems& problems) {
    LayoutNode node;
    node.Kind = ELayoutNodeKind::Split;
    const QJsonValue direction = object.value("split");
    if (direction.isString() && direction.toString() == QLatin1String("h")) {
        node.Horizontal = true;
    } else if (direction.isString() &&
               direction.toString() == QLatin1String("v")) {
        node.Horizontal = false;
    } else {
        problems.Add(path, QStringLiteral("\"split\" is \"h\" or \"v\"; \"h\" "
                                          "is used"));
    }
    std::vector<int> kept;
    node.Children =
        ParseChildren(object.value("children"),
                      path + QStringLiteral(".children"), problems, kept);
    if (node.Children.empty()) {
        problems.Add(path, QStringLiteral("a split without children is "
                                          "dropped"));
        return std::nullopt;
    }
    const QJsonValue sizes = object.value("sizes");
    const QString sizesPath = path + QStringLiteral(".sizes");
    if (!sizes.isUndefined() && !sizes.isArray()) {
        problems.Add(sizesPath, QStringLiteral("must be an array; \"fill\" is "
                                               "used for every child"));
    }
    const QJsonArray array = sizes.toArray();
    const qsizetype declared = object.value("children").toArray().size();
    if (sizes.isArray() && array.size() != declared) {
        problems.Add(sizesPath,
                     QStringLiteral("has %1 entries for %2 children; the "
                                    "missing ones are \"fill\"")
                         .arg(array.size())
                         .arg(declared));
    }
    for (const int index : kept) {
        node.Sizes.push_back(
            index < array.size()
                ? ParseSize(array.at(index),
                            QStringLiteral("%1[%2]").arg(sizesPath).arg(index),
                            problems)
                : LayoutSize::Rest());
    }
    return node;
}

std::optional<LayoutNode> ParseTabs(const QJsonObject& object,
                                    const QString& path, Problems& problems) {
    LayoutNode node;
    node.Kind = ELayoutNodeKind::Tabs;
    std::vector<int> kept;
    node.Children = ParseChildren(
        object.value("tabs"), path + QStringLiteral(".tabs"), problems, kept);
    if (node.Children.empty()) {
        problems.Add(path, QStringLiteral("a stack of tabs without tabs is "
                                          "dropped"));
        return std::nullopt;
    }
    const QJsonValue active = object.value("active");
    if (!active.isUndefined()) {
        // The index in the document, moved past the tabs that were dropped
        // before it.
        const int wanted = IsWholeNumber(active) ? active.toInt(-1) : -1;
        int found = -1;
        for (size_t i = 0; i < kept.size(); ++i) {
            if (kept[i] == wanted) {
                found = static_cast<int>(i);
            }
        }
        if (found < 0) {
            problems.Add(path + QStringLiteral(".active"),
                         QStringLiteral("is not the index of a tab; the first "
                                        "tab is shown"));
            found = 0;
        }
        node.Active = found;
    }
    return node;
}

std::optional<LayoutNode> ParseLeaf(const QJsonObject& object,
                                    const QString& path, Problems& problems) {
    LayoutNode node;
    node.Kind = ELayoutNodeKind::Leaf;
    const QJsonValue instance = object.value("instance");
    if (instance.isString()) {
        node.Instance = instance.toString();
    }
    const QJsonValue panel = object.value("panel");
    if (panel.isString()) {
        node.Panel = panel.toString();
    }
    const QJsonValue links = object.value("links");
    if (!links.isUndefined() && !links.isObject()) {
        problems.Add(path + QStringLiteral(".links"),
                     QStringLiteral("must be an object; the links are "
                                    "dropped"));
    }
    const QJsonObject linkObject = links.toObject();
    for (auto it = linkObject.begin(); it != linkObject.end(); ++it) {
        const QJsonObject link = it.value().toObject();
        const QJsonValue from = link.value("from");
        const QJsonValue output = link.value("output");
        if (!from.isString() || from.toString().isEmpty() ||
            !output.isString() || output.toString().isEmpty()) {
            problems.Add(QStringLiteral("%1.links.%2").arg(path, it.key()),
                         QStringLiteral("a link needs \"from\" and "
                                        "\"output\"; the link is dropped"));
            continue;
        }
        node.Links.insert(it.key(), {.From = from.toString(),
                                     .Output = output.toString(),
                                     .Problem = {}});
    }
    return node;
}

std::optional<LayoutNode> ParseNode(const QJsonValue& value,
                                    const QString& path, Problems& problems) {
    if (!value.isObject()) {
        problems.Add(path, QStringLiteral("a node is an object; dropped"));
        return std::nullopt;
    }
    const QJsonObject object = value.toObject();
    if (object.contains("split")) {
        return ParseSplit(object, path, problems);
    }
    if (object.contains("tabs")) {
        return ParseTabs(object, path, problems);
    }
    if (object.contains("panel") || object.contains("instance")) {
        return ParseLeaf(object, path, problems);
    }
    problems.Add(path, QStringLiteral("neither a split, a stack of tabs nor a "
                                      "panel; dropped"));
    return std::nullopt;
}

// Checking: instance ids, panel types, links.

struct Resolver {
    const LayoutKnownPanels& Known;
    Problems& Report;
    // Instance id -> the panel type of its leaf; the first leaf of an id owns
    // it.
    QMap<QString, QString> Panels;
    QSet<QString> Used;

    // Pass 0: every explicit id is reserved, so a renamed leaf never takes the
    // id another leaf wrote.
    void Reserve(const LayoutNode& node) {
        if (node.Kind == ELayoutNodeKind::Leaf) {
            if (!node.Instance.isEmpty()) {
                Used.insert(node.Instance);
            }
            return;
        }
        for (const LayoutNode& child : node.Children) {
            Reserve(child);
        }
    }

    // Pass 1: ids unique and non-empty, in document order.
    void Name(LayoutNode& node, const QString& path) {
        if (node.Kind != ELayoutNodeKind::Leaf) {
            for (size_t i = 0; i < node.Children.size(); ++i) {
                Name(node.Children[i],
                     QStringLiteral("%1.%2[%3]")
                         .arg(path, node.Kind == ELayoutNodeKind::Split
                                        ? QStringLiteral("children")
                                        : QStringLiteral("tabs"))
                         .arg(i));
            }
            return;
        }
        const QString wanted =
            node.Instance.isEmpty()
                ? (node.Panel.isEmpty() ? QStringLiteral("panel") : node.Panel)
                : node.Instance;
        if (!node.Instance.isEmpty() && !Panels.contains(wanted)) {
            Panels.insert(wanted, node.Panel);
            return;
        }
        QString name = wanted;
        for (int n = 2; Used.contains(name); ++n) {
            name = QStringLiteral("%1-%2").arg(wanted).arg(n);
        }
        Report.Add(
            path,
            node.Instance.isEmpty()
                ? QStringLiteral("no instance id; \"%1\" is used").arg(name)
                : QStringLiteral("instance \"%1\" is used twice; "
                                 "this one is \"%2\"")
                      .arg(wanted, name));
        node.Instance = name;
        Used.insert(name);
        Panels.insert(name, node.Panel);
    }

    // Pass 2: stubs and links.
    void Check(LayoutNode& node, const QString& path) {
        if (node.Kind != ELayoutNodeKind::Leaf) {
            for (size_t i = 0; i < node.Children.size(); ++i) {
                Check(node.Children[i],
                      QStringLiteral("%1.%2[%3]")
                          .arg(path, node.Kind == ELayoutNodeKind::Split
                                         ? QStringLiteral("children")
                                         : QStringLiteral("tabs"))
                          .arg(i));
            }
            return;
        }
        const auto own = Known.constFind(node.Panel);
        if (own == Known.constEnd()) {
            node.Error = node.Panel.isEmpty()
                             ? QStringLiteral("no panel type")
                             : QStringLiteral("no panel %1").arg(node.Panel);
            Report.Add(path, node.Error);
        }
        for (auto it = node.Links.begin(); it != node.Links.end(); ++it) {
            it->Problem = LinkProblem(node, own, it.key(), it.value());
            if (!it->Problem.isEmpty()) {
                Report.Add(
                    QStringLiteral("%1.links.%2").arg(path, it.key()),
                    QStringLiteral("%1; the link is off").arg(it->Problem));
            }
        }
    }

    QString LinkProblem(const LayoutNode& node,
                        LayoutKnownPanels::const_iterator own,
                        const QString& input, const LayoutLink& link) const {
        if (own == Known.constEnd()) {
            return QStringLiteral("the panel %1 is unknown").arg(node.Panel);
        }
        if (!own->Inputs.contains(input)) {
            return QStringLiteral("%1 has no input %2").arg(node.Panel, input);
        }
        if (link.From == node.Instance) {
            return QStringLiteral("a panel cannot feed itself");
        }
        const auto source = Panels.constFind(link.From);
        if (source == Panels.constEnd()) {
            return QStringLiteral("there is no instance %1").arg(link.From);
        }
        const auto sourcePanel = Known.constFind(source.value());
        if (sourcePanel == Known.constEnd()) {
            return QStringLiteral("the panel %1 of %2 is unknown")
                .arg(source.value(), link.From);
        }
        if (!sourcePanel->Outputs.contains(link.Output)) {
            return QStringLiteral("%1 has no output %2")
                .arg(source.value(), link.Output);
        }
        const QString outType =
            sourcePanel->Outputs.value(link.Output).toString();
        const QString inType = own->Inputs.value(input).toString();
        if (outType != inType) {
            return QStringLiteral("the output %1 is %2, the input %3 is %4")
                .arg(link.Output, outType, input, inType);
        }
        return {};
    }
};

LayoutParseResult Resolved(Layout tree, QStringList problems, bool writable,
                           const LayoutKnownPanels& known) {
    Problems report;
    Resolver resolver{
        .Known = known, .Report = report, .Panels = {}, .Used = {}};
    resolver.Reserve(tree.Root);
    resolver.Name(tree.Root, QStringLiteral("root"));
    resolver.Check(tree.Root, QStringLiteral("root"));
    problems.append(report.Take());
    return {.Tree = std::move(tree),
            .Problems = std::move(problems),
            .Writable = writable};
}

QJsonValue SizeToJson(const LayoutSize& size) {
    return size.Fill ? QJsonValue(QLatin1String(FILL))
                     : QJsonValue(size.Pixels);
}

QJsonObject NodeToJson(const LayoutNode& node) {
    QJsonObject object;
    switch (node.Kind) {
        case ELayoutNodeKind::Split: {
            QJsonArray children;
            QJsonArray sizes;
            for (const LayoutNode& child : node.Children) {
                children.append(NodeToJson(child));
            }
            for (const LayoutSize& size : node.Sizes) {
                sizes.append(SizeToJson(size));
            }
            object.insert("split", node.Horizontal ? "h" : "v");
            object.insert("children", children);
            object.insert("sizes", sizes);
            break;
        }
        case ELayoutNodeKind::Tabs: {
            QJsonArray tabs;
            for (const LayoutNode& child : node.Children) {
                tabs.append(NodeToJson(child));
            }
            object.insert("tabs", tabs);
            object.insert("active", node.Active);
            break;
        }
        case ELayoutNodeKind::Leaf: {
            QJsonObject links;
            for (auto it = node.Links.constBegin(); it != node.Links.constEnd();
                 ++it) {
                links.insert(it.key(), QJsonObject{{"from", it->From},
                                                   {"output", it->Output}});
            }
            object.insert("instance", node.Instance);
            object.insert("panel", node.Panel);
            object.insert("links", links);
            break;
        }
    }
    return object;
}

}  // namespace

Layout DefaultLayout() {
    return {.Root = DefaultRoot(QStringLiteral("sessions.list"),
                                QStringLiteral("terminal.view"),
                                QStringLiteral("session"))};
}

QStringList ListPanelTypes(const QStringList& order,
                           const LayoutKnownPanels& known) {
    QStringList lists;
    for (const QString& type : order) {
        const auto found = known.constFind(type);
        if (found != known.cend() &&
            found->Outputs.value(QStringLiteral("selected")).toString() ==
                QStringLiteral("SessionRef")) {
            lists.append(type);
        }
    }
    return lists;
}

DefaultLayoutResult DefaultLayoutFor(const QStringList& order,
                                     const LayoutKnownPanels& known) {
    const QString sessionRef = QStringLiteral("SessionRef");
    const QStringList lists = ListPanelTypes(order, known);
    DefaultLayoutResult result{.Tree = DefaultLayout(), .Problems = {}};
    QString listPanel = QStringLiteral("sessions.list");
    if (!lists.isEmpty()) {
        listPanel = lists.first();
        if (lists.size() > 1) {
            result.Problems.append(
                QStringLiteral(
                    "default layout: several panels declare the output "
                    "selected of type SessionRef; %1 is used, %2 not placed "
                    "(disable the one you do not want in settings, "
                    "plugins.disabled)")
                    .arg(listPanel, lists.mid(1).join(QLatin1String(", "))));
        }
    }
    // A terminal takes the session in its one input of that type; a panel
    // with several is not told from the others by its type, and the list is
    // not its own terminal.
    QStringList terminals;
    QString terminalInput = QStringLiteral("session");
    QMap<QString, QString> inputOf;
    for (const QString& type : order) {
        const auto found = known.constFind(type);
        if (found == known.cend() || type == listPanel) {
            continue;
        }
        const QStringList inputs = NamesOfType(found->Inputs, sessionRef);
        if (inputs.size() == 1) {
            terminals.append(type);
            inputOf.insert(type, inputs.first());
        }
    }
    QString terminalPanel = QStringLiteral("terminal.view");
    if (!terminals.isEmpty()) {
        terminalPanel = terminals.first();
        terminalInput = inputOf.value(terminalPanel);
        if (terminals.size() > 1) {
            result.Problems.append(
                QStringLiteral(
                    "default layout: several panels take a SessionRef input; "
                    "%1 is used, %2 not placed")
                    .arg(terminalPanel,
                         terminals.mid(1).join(QLatin1String(", "))));
        }
    }
    result.Tree = {.Root =
                       DefaultRoot(listPanel, terminalPanel, terminalInput)};
    return result;
}

LayoutParseResult ParseLayout(const QJsonValue& document,
                              const LayoutKnownPanels& known,
                              const Layout& fallback) {
    if (!document.isObject()) {
        return Resolved(fallback,
                        {QStringLiteral("the layout is not an object; the "
                                        "default layout is used")},
                        true, known);
    }
    const QJsonObject object = document.toObject();
    // As the `sessions` document: no version or the integer 1 is read and
    // written; anything else belongs to another roost.
    const QJsonValue version = object.value("version");
    if (!version.isUndefined() &&
        !(IsWholeNumber(version) && version.toInt() == LAYOUT_VERSION)) {
        return Resolved(fallback,
                        {QStringLiteral("the layout has a version this roost "
                                        "does not know; the default layout is "
                                        "used and the stored one is kept")},
                        false, known);
    }
    Problems problems;
    std::optional<LayoutNode> root =
        ParseNode(object.value("root"), QStringLiteral("root"), problems);
    QStringList lines = problems.Take();
    if (!root) {
        lines.append(
            QStringLiteral("root: no usable root; the default layout "
                           "is used"));
        return Resolved(fallback, std::move(lines), true, known);
    }
    Layout tree{.Root = std::move(*root)};
    // Optional, and no reason to refuse the document: a value of another type
    // is the same as none.
    const QJsonValue hidden = object.value("listsHidden");
    if (hidden.isBool()) {
        tree.ListsHidden = hidden.toBool();
    } else if (!hidden.isUndefined() && !hidden.isNull()) {
        lines.append(
            QStringLiteral("listsHidden: must be true or false; "
                           "the lists are shown"));
    }
    return Resolved(std::move(tree), std::move(lines), true, known);
}

QJsonObject LayoutToJson(const Layout& layout) {
    QJsonObject document{{"version", LAYOUT_VERSION},
                         {"root", NodeToJson(layout.Root)}};
    if (layout.ListsHidden) {
        document.insert("listsHidden", true);
    }
    return document;
}

}  // namespace roost
