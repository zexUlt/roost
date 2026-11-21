#include "roost/shell.hpp"

#include <QJSValue>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QMetaObject>
#include <QSet>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtLogging>

#include <algorithm>
#include <utility>

#include "core/protocol.hpp"
#include "core/version.hpp"
#include "roost/layout_commands.hpp"

namespace roost {

namespace {

constexpr const char* LIST_PANEL = "sessions.list";
constexpr const char* TERMINAL_PANEL = "terminal.view";
// The ids of the two panels of the window of a root without a layout view.
constexpr const char* LIST_INSTANCE = "list";
constexpr const char* TERMINAL_INSTANCE = "terminal";
constexpr const char* SESSION_REF = "SessionRef";

// The name of the one entry of `declared` that has the type, or empty with the
// reason in `problem` if there is none or several.
QString SingleOfType(const QVariantMap& declared, const QString& type,
                     const QString& panel, const char* kind, QString& problem) {
    QStringList names;
    for (auto it = declared.begin(); it != declared.end(); ++it) {
        if (it.value().toString() == type) {
            names.append(it.key());
        }
    }
    if (names.size() == 1) {
        return names.first();
    }
    problem = QStringLiteral(
                  "panel %1 declares %2 %3 of type %4, the link "
                  "needs exactly one; the link is off")
                  .arg(panel)
                  .arg(names.isEmpty() ? QStringLiteral("no")
                                       : QString::number(names.size()))
                  .arg(QLatin1String(kind), type);
    return {};
}

// Whether the list panel has not yet settled what it opens at start. A panel
// without the optional property `restored` is not waited for.
bool ListIsRestoring(const QQuickItem* panel) {
    if (panel == nullptr ||
        panel->metaObject()->indexOfProperty("restored") < 0) {
        return false;
    }
    return !panel->property("restored").toBool();
}

// What the terminal panel does with the session the link gave it.
struct TerminalOpening {
    // The panel shows a session ("agent/id") through a TerminalItem.
    bool Wanted = false;
    QString Key;
    // The status of the item: "none", "opening", "open", "ended", "failed" or
    // "disconnected", and its reason.
    QString Status;
    QString Message;
};

TerminalOpening OpeningOf(const QQuickItem* panel) {
    TerminalOpening opening;
    if (panel == nullptr) {
        return opening;
    }
    const auto* item = panel->findChild<const QQuickItem*>("terminal");
    if (item == nullptr || item->metaObject()->indexOfProperty("status") < 0) {
        return opening;
    }
    QVariant session = panel->property("session");
    if (session.metaType() == QMetaType::fromType<QJSValue>()) {
        session = session.value<QJSValue>().toVariant();
    }
    const QVariantMap ref = session.toMap();
    if (ref.isEmpty()) {
        return opening;
    }
    opening.Wanted = true;
    opening.Key = QStringLiteral("%1/%2").arg(ref.value("agent").toString(),
                                              ref.value("id").toString());
    opening.Status = item->property("status").toString();
    opening.Message = item->property("statusMessage").toString();
    return opening;
}

// The leaves of the tree, by instance id, in document order.
void CollectLeaves(const LayoutNode& node,
                   QList<std::pair<QString, QString>>& leaves) {
    if (node.Kind == ELayoutNodeKind::Leaf) {
        leaves.append({node.Instance, node.Panel});
        return;
    }
    for (const LayoutNode& child : node.Children) {
        CollectLeaves(child, leaves);
    }
}

// The tree as the layout view takes it (LayoutView.qml): a size is a number of
// pixels, 0 for the rest.
QVariant NodeToVariant(const LayoutNode& node) {
    QVariantMap map;
    QVariantList children;
    for (const LayoutNode& child : node.Children) {
        children.append(NodeToVariant(child));
    }
    switch (node.Kind) {
        case ELayoutNodeKind::Split: {
            QVariantList sizes;
            for (const LayoutSize& size : node.Sizes) {
                sizes.append(size.Fill ? 0 : size.Pixels);
            }
            map = {{"kind", "split"},
                   {"horizontal", node.Horizontal},
                   {"children", children},
                   {"sizes", sizes}};
            break;
        }
        case ELayoutNodeKind::Tabs:
            map = {{"kind", "tabs"},
                   {"active", node.Active},
                   {"children", children}};
            break;
        case ELayoutNodeKind::Leaf:
            map = {{"kind", "leaf"},
                   {"instance", node.Instance},
                   {"panel", node.Panel}};
            break;
    }
    return map;
}

// A list that QML gave as a `var`, whichever way it arrives.
QVariantList AsList(const QVariant& value) {
    if (value.metaType() == QMetaType::fromType<QJSValue>()) {
        return value.value<QJSValue>().toVariant().toList();
    }
    return value.toList();
}

QVariantMap AsMap(const QVariant& value) {
    if (value.metaType() == QMetaType::fromType<QJSValue>()) {
        return value.value<QJSValue>().toVariant().toMap();
    }
    return value.toMap();
}

QQuickItem* AsItem(const QVariant& value) {
    if (value.metaType() == QMetaType::fromType<QJSValue>()) {
        return qobject_cast<QQuickItem*>(value.value<QJSValue>().toQObject());
    }
    return qobject_cast<QQuickItem*>(value.value<QObject*>());
}

// The patch that makes the stored document the tree: the document itself, and
// null for the keys of another kind of root that may still be stored (arrays
// are replaced by a merge patch, so nothing below the root can be left over).
QJsonObject LayoutPatch(const Layout& layout) {
    QJsonObject patch = LayoutToJson(layout);
    QJsonObject root = patch.value("root").toObject();
    for (const char* key : {"split", "children", "sizes", "tabs", "active",
                            "instance", "panel", "links"}) {
        if (!root.contains(QLatin1String(key))) {
            root.insert(QLatin1String(key), QJsonValue::Null);
        }
    }
    patch.insert("root", root);
    return patch;
}

// The lines of a parse that the shell shows: the structure of the document.
// Unknown panel types and links are the shell's own (the slot's placeholder
// and the link problems), worked out from the panels there are.
QStringList StructureProblems(const QStringList& lines) {
    QStringList kept;
    for (const QString& line : lines) {
        if (line.contains(QLatin1String(".links.")) ||
            line.contains(QLatin1String(": no panel "))) {
            continue;
        }
        kept.append(line);
    }
    return kept;
}

}  // namespace

Shell::Shell(QObject& root, PluginLoader& loader, api::Api& api,
             QString socketPath, DaemonLauncher* launcher)
    : Root_(root),
      Loader_(loader),
      Api_(api),
      SocketPath_(std::move(socketPath)),
      Launcher_(launcher) {
    LayoutMode_ = root.metaObject()->indexOfProperty("layoutTree") >= 0;
    if (LayoutMode_) {
        connect(&Root_, SIGNAL(slotCreated(QVariant)), this,
                SLOT(OnSlotCreated(QVariant)));
        connect(&Root_, SIGNAL(splitResized(QVariant, QVariant)), this,
                SLOT(OnSplitResized(QVariant, QVariant)));
        connect(&Root_, SIGNAL(layoutEdit(QString, QVariant)), this,
                SLOT(OnLayoutEdit(QString, QVariant)));
    } else {
        for (auto* item : root.findChildren<QQuickItem*>("panelSlot")) {
            Slots_.append({.Item = item,
                           .Type = item->property("panelType").toString(),
                           .Instance = {},
                           .Panel = {},
                           .LinkProblems = {},
                           .Links = {},
                           .LinkInput = {},
                           .LinkOutput = {}});
        }
        if (Slots_.isEmpty()) {
            qWarning("roost: the window has no panel slot");
        }
    }

    connect(&Loader_, &PluginLoader::PanelTypesChanged, this,
            &Shell::OnPanelTypesChanged);
    connect(&Loader_, &PluginLoader::ProblemsChanged, this,
            &Shell::UpdateProblems);
    // Direct: after the signal the item is gone.
    connect(&Loader_, &PluginLoader::PanelAboutToBeDestroyed, this,
            &Shell::OnPanelAboutToBeDestroyed, Qt::DirectConnection);
    connect(&Api_.Plugins(), &api::PluginService::Changed, this,
            &Shell::OnPanelTypesChanged);
    connect(&Api_, &api::Api::Connected, this, [this] {
        SetConnection(QStringLiteral("connected"), {});
        ReadLayout();
    });
    connect(&Api_, &api::Api::Disconnected, this, [this](const QString& why) {
        // A refusal is followed by a Disconnected; the refusal is what the
        // window shows until Retry or a connection.
        if (Connection_ != QLatin1String("refused")) {
            SetConnection(QStringLiteral("disconnected"), why);
        }
    });
    connect(&Api_, &api::Api::Refused, this, &Shell::OnRefused);
    if (Launcher_ != nullptr) {
        // Only the text of the banner follows the launcher.
        const auto refresh = [this] {
            SetConnection(Connection_, ConnectionMessage_);
        };
        connect(Launcher_, &DaemonLauncher::StartingChanged, this, refresh);
        connect(Launcher_, &DaemonLauncher::FailureChanged, this, refresh);
        connect(Launcher_, &DaemonLauncher::RestartingChanged, this, refresh);
        // The new roostd is coming up: connect as Retry does.
        connect(Launcher_, &DaemonLauncher::Restarted, this, &Shell::OnRetry);
    }
    connect(&Root_, SIGNAL(retryRequested()), this, SLOT(OnRetry()));
    connect(&Root_, SIGNAL(restartRequested()), this,
            SLOT(OnRestartRequested()));

    SetConnection(Api_.IsConnected() ? QStringLiteral("connected")
                                     : QStringLiteral("connecting"),
                  {});
    if (LayoutMode_) {
        // The default layout until the stored one is known: the slots wait.
        ApplyLayout(DefaultLayout());
        ReadLayout();
    }
    Refill();
    UpdateProblems();
}

Shell::~Shell() {
    disconnect(&Loader_, nullptr, this, nullptr);
    for (Slot& slot : Slots_) {
        QQuickItem* panel = slot.Panel.data();
        if (panel == nullptr) {
            continue;
        }
        Write(slot, "panel", QVariant::fromValue<QQuickItem*>(nullptr));
        slot.Panel.clear();
        Loader_.DestroyPanel(panel);
    }
}

Shell::Readiness Shell::CheckReadiness() const {
    QStringList failures;
    for (const auto& problem : Loader_.Problems()) {
        failures.append(
            problem.Panel.isEmpty()
                ? QStringLiteral("plugin %1: %2")
                      .arg(problem.Plugin, problem.Message)
                : QStringLiteral("plugin %1, panel %2: %3")
                      .arg(problem.Plugin, problem.Panel, problem.Message));
    }
    for (const Slot& slot : Slots_) {
        failures.append(slot.LinkProblems);
    }
    if (Connection_ == QLatin1String("refused")) {
        failures.append(QStringLiteral("roostd refused the connection"));
    }
    // What roostd found wrong with the plugins (a manifest it could not read).
    const auto listed = Api_.Plugins().Plugins().value("problems").toArray();
    for (const auto& problem : listed) {
        failures.append(QStringLiteral("roostd: %1").arg(problem.toString()));
    }
    // A missing slot cannot appear by waiting. A layout shows what it has: it
    // may have no terminal.
    if (!LayoutMode_) {
        for (const char* type : {LIST_PANEL, TERMINAL_PANEL}) {
            const bool present =
                std::ranges::any_of(Slots_, [type](const Slot& slot) {
                    return slot.Type == QLatin1String(type);
                });
            if (!present) {
                failures.append(
                    QStringLiteral("the window has no panel %1").arg(type));
            }
        }
    }
    // roostd refused the open: the attach will not start by waiting.
    QList<std::pair<const Slot*, TerminalOpening>> openings;
    for (const Slot& slot : Slots_) {
        if (!TakesSession(slot)) {
            continue;
        }
        const TerminalOpening opening = OpeningOf(slot.Panel.data());
        openings.append({&slot, opening});
        if (opening.Wanted && opening.Status == QLatin1String("failed")) {
            failures.append(QStringLiteral("panel %1 could not open %2: %3")
                                .arg(slot.Type, opening.Key, opening.Message));
        }
    }
    if (!failures.isEmpty()) {
        return {false, true, failures.join(QStringLiteral("; "))};
    }

    QStringList waiting;
    if (!Api_.IsConnected()) {
        waiting.append(
            QStringLiteral("not connected to roostd at %1").arg(SocketPath_));
    }
    if (LayoutMode_ && !LayoutSettled_) {
        waiting.append(QStringLiteral("the layout is not loaded"));
    }
    for (const Slot& slot : Slots_) {
        if (slot.Panel.isNull()) {
            waiting.append(QStringLiteral("panel %1 is not created (%2)")
                               .arg(slot.Type, ErrorFor(slot.Type)));
        }
    }
    const auto& sessions = Api_.Sessions();
    if (!sessions.IsLoaded()) {
        waiting.append(QStringLiteral("sessions are not loaded"));
    } else if (sessions.Rows().isEmpty()) {
        waiting.append(QStringLiteral("roostd lists no sessions"));
    }
    // The window opens a session at start, the stored one that is still
    // listed. Whether it does is known once the list panel has restored it;
    // then the terminal has to be open (roostd answered session.open, so the
    // attach process is started, or was already for a warm session) or, if
    // the program already left, ended.
    const Slot* restoring = nullptr;
    for (const Slot& slot : Slots_) {
        if (restoring == nullptr && IsListSlot(slot) &&
            ListIsRestoring(slot.Panel.data())) {
            restoring = &slot;
        }
    }
    if (restoring != nullptr) {
        waiting.append(
            QStringLiteral("panel %1 has not restored the opened session")
                .arg(restoring->Type));
    } else {
        for (const auto& [slot, opening] : openings) {
            if (opening.Wanted && opening.Status != QLatin1String("open") &&
                opening.Status != QLatin1String("ended")) {
                waiting.append(
                    QStringLiteral("panel %1 is opening %2 (%3)")
                        .arg(slot->Type, opening.Key, opening.Status));
            }
        }
    }
    if (!waiting.isEmpty()) {
        return {false, false, waiting.join(QStringLiteral("; "))};
    }
    return {true, false, {}};
}

Shell::Slot* Shell::SlotOf(const QString& instance) {
    for (Slot& slot : Slots_) {
        if (slot.Instance == instance) {
            return &slot;
        }
    }
    return nullptr;
}

bool Shell::IsListSlot(const Slot& slot) const {
    return ListPanelTypes(Loader_.PanelTypes(), KnownPanels())
        .contains(slot.Type);
}

bool Shell::TakesSession(const Slot& slot) const {
    const auto info = Loader_.Info(slot.Type);
    if (!info) {
        return false;
    }
    const auto isSession = [&info](const QString& input) {
        return info->Inputs.value(input).toString() ==
               QLatin1String(SESSION_REF);
    };
    if (isSession(slot.LinkInput)) {
        return true;
    }
    return std::ranges::any_of(slot.Links, [&isSession](const QVariant& link) {
        return isSession(link.toMap().value("input").toString());
    });
}

void Shell::UpdateListTypes() {
    if (!LayoutMode_) {
        return;
    }
    const QStringList types =
        ListPanelTypes(Loader_.PanelTypes(), KnownPanels());
    if (Root_.property("listPanelTypes").toStringList() != types) {
        Root_.setProperty("listPanelTypes", types);
    }
}

void Shell::OnPanelTypesChanged() {
    UpdateListTypes();
    PublishPanelTypes();
    TrySettle();
    Refill();
    UpdateLinks();
    UpdateProblems();
}

void Shell::OnPanelAboutToBeDestroyed(QQuickItem* panel) {
    for (Slot& slot : Slots_) {
        if (slot.Panel != panel) {
            continue;
        }
        // The slot first: its Binding lets go of the item while it lives.
        // `panel` must be written null before `linkInput` and `linkOutput`
        // are cleared: an empty `property` on a live target makes the Binding
        // warn. The Binding does not restore an old property when only its
        // name changes; Shell never does that, the names change only with the
        // panel.
        Write(slot, "panel", QVariant::fromValue<QQuickItem*>(nullptr));
        slot.Panel.clear();
        slot.LinkProblems.clear();
        slot.Links.clear();
        slot.LinkInput.clear();
        slot.LinkOutput.clear();
        Write(slot, "links", QVariantList());
        Write(slot, "linkInput", QString());
        Write(slot, "linkOutput", QString());
        Write(slot, "error", ErrorFor(slot.Type));
    }
    UpdateLinks();
    UpdateProblems();
}

void Shell::Refill() {
    if (Filling_) {
        // CreatePanel emitted PanelTypesChanged; the outer loop goes again.
        FillAgain_ = true;
        return;
    }
    Filling_ = true;
    do {
        FillAgain_ = false;
        for (Slot& slot : Slots_) {
            if (slot.Item && slot.Panel.isNull()) {
                Fill(slot);
            }
        }
    } while (FillAgain_);
    Filling_ = false;
    UpdateLinks();
}

void Shell::Fill(Slot& slot) {
    const auto info = Loader_.Info(slot.Type);
    // No panel before the layout is known: a leaf of the stored layout may not
    // be one of the default.
    const bool known = !LayoutMode_ || LayoutSettled_;
    QString instance = slot.Instance;
    if (instance.isEmpty()) {
        instance = QLatin1String(slot.Type == QLatin1String(LIST_PANEL)
                                     ? LIST_INSTANCE
                                     : TERMINAL_INSTANCE);
    }
    QQuickItem* panel =
        info && known ? Loader_.CreatePanel(slot.Type, instance) : nullptr;
    if (panel == nullptr) {
        Write(slot, "error", ErrorFor(slot.Type));
        return;
    }
    slot.Panel = panel;
    if (slot.Instance.isEmpty()) {
        SetLegacyLink(slot, *info);
    }
    Write(slot, "error", QString());
    // Last: the slot reparents the panel, and its link becomes active.
    Write(slot, "panel", QVariant::fromValue<QQuickItem*>(panel));
}

QString Shell::ErrorFor(const QString& type) const {
    if (Connection_ == QLatin1String("refused")) {
        return QStringLiteral("roostd refused the connection");
    }
    if (!Api_.Plugins().IsLoaded()) {
        return QStringLiteral("waiting for roostd");
    }
    for (const auto& problem : Loader_.Problems()) {
        if (problem.Panel == type) {
            return problem.Message;
        }
    }
    return QStringLiteral("no panel %1").arg(type);
}

void Shell::SetLegacyLink(Slot& slot, const PluginLoader::PanelInfo& info) {
    slot.LinkProblems.clear();
    QString problem;
    QString input;
    QString output;
    const bool isList = slot.Type == QLatin1String(LIST_PANEL);
    if (isList) {
        output = SingleOfType(info.Outputs, QLatin1String(SESSION_REF),
                              slot.Type, "output", problem);
    } else if (slot.Type == QLatin1String(TERMINAL_PANEL)) {
        input = SingleOfType(info.Inputs, QLatin1String(SESSION_REF), slot.Type,
                             "input", problem);
    }
    if (!problem.isEmpty()) {
        slot.LinkProblems.append(problem);
    }
    // A declared name without a property is the loader's problem; the link
    // would only write to nothing.
    const QMetaObject* meta = slot.Panel->metaObject();
    const QString name = isList ? output : input;
    if (!name.isEmpty() && meta->indexOfProperty(qPrintable(name)) < 0) {
        output.clear();
        input.clear();
    }
    Write(slot, "linkInput", input);
    Write(slot, "linkOutput", output);
}

void Shell::UpdateLinks() {
    if (!LayoutMode_) {
        return;
    }
    QMap<QString, QString> outputs;
    for (Slot& dest : Slots_) {
        const LayoutNode* leaf = FindLeaf(Layout_.Root, dest.Instance);
        const auto info = Loader_.Info(dest.Type);
        QStringList problems;
        QVariantList links;
        QString linkInput;
        if (leaf != nullptr && info && dest.Panel) {
            for (auto it = leaf->Links.cbegin(); it != leaf->Links.cend();
                 ++it) {
                const QString& input = it.key();
                const Slot* source = SlotOf(it->From);
                if (source == nullptr || source->Panel.isNull()) {
                    continue;
                }
                const auto sourceInfo = Loader_.Info(source->Type);
                if (!sourceInfo) {
                    continue;
                }
                if (!info->Inputs.contains(input)) {
                    problems.append(
                        QStringLiteral("panel %1 declares no input %2; the "
                                       "link is off")
                            .arg(dest.Type, input));
                    continue;
                }
                const QString type = info->Inputs.value(input).toString();
                QString output = it->Output;
                if (sourceInfo->Outputs.value(output).toString() != type) {
                    QString problem;
                    output = SingleOfType(sourceInfo->Outputs, type,
                                          source->Type, "output", problem);
                    if (output.isEmpty()) {
                        problems.append(problem);
                        continue;
                    }
                }
                // A declared name without a property is the loader's problem;
                // the link would only write to nothing.
                if (dest.Panel->metaObject()->indexOfProperty(
                        qPrintable(input)) < 0 ||
                    source->Panel->metaObject()->indexOfProperty(
                        qPrintable(output)) < 0) {
                    continue;
                }
                links.append(QVariantMap{
                    {"input", input}, {"from", it->From}, {"output", output}});
                if (linkInput.isEmpty()) {
                    linkInput = input;
                }
                if (!outputs.contains(it->From)) {
                    outputs.insert(it->From, output);
                }
            }
        }
        dest.LinkProblems = problems;
        if (dest.Links != links) {
            dest.Links = links;
            Write(dest, "links", links);
        }
        if (dest.LinkInput != linkInput) {
            dest.LinkInput = linkInput;
            Write(dest, "linkInput", linkInput);
        }
    }
    for (Slot& source : Slots_) {
        const QString output = outputs.value(source.Instance);
        if (source.LinkOutput != output) {
            source.LinkOutput = output;
            Write(source, "linkOutput", output);
        }
    }
}

void Shell::OnSlotCreated(const QVariant& value) {
    QQuickItem* item = AsItem(value);
    if (item == nullptr) {
        return;
    }
    Slot* slot = SlotOf(item->property("instanceId").toString());
    if (slot == nullptr) {
        return;
    }
    // A slot of an earlier tree is left as it is: it is deleted, and writing
    // to it would let go of the links of the panel that moves to the new one.
    slot->Item = item;
    slot->Links.clear();
    slot->LinkInput.clear();
    slot->LinkOutput.clear();
    if (slot->Panel) {
        Write(*slot, "error", QString());
        Write(*slot, "panel", QVariant::fromValue<QQuickItem*>(slot->Panel));
    } else {
        Write(*slot, "error", ErrorFor(slot->Type));
        // The panel is made outside the creation of the layout view's items.
        QMetaObject::invokeMethod(this, &Shell::Refill, Qt::QueuedConnection);
    }
    UpdateLinks();
}

void Shell::OnSplitResized(const QVariant& path, const QVariant& sizes) {
    LayoutNode* node = &Layout_.Root;
    for (const QVariant& index : AsList(path)) {
        const int at = index.toInt();
        if (node->Kind == ELayoutNodeKind::Leaf || at < 0 ||
            static_cast<size_t>(at) >= node->Children.size()) {
            return;
        }
        node = &node->Children[static_cast<size_t>(at)];
    }
    const QVariantList list = AsList(sizes);
    if (node->Kind != ELayoutNodeKind::Split ||
        static_cast<size_t>(list.size()) != node->Children.size()) {
        return;
    }
    std::vector<LayoutSize> changed;
    for (const QVariant& size : list) {
        changed.push_back(size.toInt() > 0 ? LayoutSize::Px(size.toInt())
                                           : LayoutSize::Rest());
    }
    if (changed == node->Sizes) {
        return;
    }
    node->Sizes = std::move(changed);
    SaveLayout();
}

void Shell::ReadLayout() {
    if (!LayoutMode_ || LayoutRequested_ || !Api_.IsConnected()) {
        return;
    }
    LayoutRequested_ = true;
    Api_.Store()
        .Get(QStringLiteral("layout"))
        .then(this, [this](rpc::Result answer) {
            LayoutAnswered_ = true;
            if (answer) {
                LayoutDocument_ =
                    answer->toObject().value(QStringLiteral("document"));
            } else {
                // Not overwritten: what is stored may be fine.
                LayoutWritable_ = false;
                LayoutProblems_.append(
                    QStringLiteral("the layout could not be read (%1); the "
                                   "default layout is used")
                        .arg(answer.error().Message));
            }
            TrySettle();
            Refill();
            UpdateProblems();
        });
}

void Shell::TrySettle() {
    if (!LayoutMode_ || LayoutSettled_ || !LayoutAnswered_ ||
        !Api_.Plugins().IsLoaded()) {
        return;
    }
    const LayoutKnownPanels known = KnownPanels();
    // The default layout is made of the panels there are, by their ports.
    DefaultLayoutResult standard =
        DefaultLayoutFor(Loader_.PanelTypes(), known);
    // No document is the default layout, and says nothing about itself.
    LayoutParseResult parsed{
        .Tree = standard.Tree, .Problems = standard.Problems, .Writable = true};
    if (LayoutDocument_.isObject() && !LayoutDocument_.toObject().isEmpty()) {
        parsed = ParseLayout(LayoutDocument_, known, standard.Tree);
    }
    LayoutSettled_ = true;
    LayoutWritable_ = LayoutWritable_ && parsed.Writable;
    LayoutProblems_.append(StructureProblems(parsed.Problems));
    ApplyLayout(std::move(parsed.Tree));
}

void Shell::SetLayout(const Layout& layout) {
    if (!LayoutMode_) {
        return;
    }
    ApplyLayout(layout);
    SaveLayout();
}

void Shell::ApplyLayout(Layout layout) {
    QList<std::pair<QString, QString>> leaves;
    CollectLeaves(layout.Root, leaves);
    QMap<QString, QString> wanted;
    for (const auto& [instance, type] : leaves) {
        wanted.insert(instance, type);
    }
    // The panels of the leaves that go, or whose type changed, are given back
    // first.
    for (qsizetype i = Slots_.size() - 1; i >= 0; --i) {
        const Slot& slot = Slots_[i];
        const auto found = wanted.constFind(slot.Instance);
        if (found != wanted.cend() && found.value() == slot.Type) {
            continue;
        }
        if (QQuickItem* panel = slot.Panel.data()) {
            Loader_.DestroyPanel(panel);
        }
        Slots_.removeAt(i);
    }
    const bool changed = !LayoutPublished_ || Layout_ != layout;
    Layout_ = std::move(layout);
    for (const auto& [instance, type] : leaves) {
        if (SlotOf(instance) == nullptr) {
            Slots_.append({.Item = {},
                           .Type = type,
                           .Instance = instance,
                           .Panel = {},
                           .LinkProblems = {},
                           .Links = {},
                           .LinkInput = {},
                           .LinkOutput = {}});
        }
    }
    if (changed) {
        PublishLayout();
    }
    Refill();
    UpdateProblems();
}

void Shell::PublishLayout() {
    LayoutPublished_ = true;
    Root_.setProperty("layoutTree", NodeToVariant(Layout_.Root));
}

void Shell::SaveLayout() {
    if (!LayoutSettled_) {
        return;
    }
    if (!LayoutWritable_) {
        // The edit stays in the window; the user is told it will not last.
        WriteProblem_ = QStringLiteral(
            "the stored layout is not writable: edits are not saved");
        UpdateProblems();
        return;
    }
    Api_.Store()
        .Patch(QStringLiteral("layout"), LayoutPatch(Layout_))
        .then(this, [this](rpc::Result answer) {
            const QString problem =
                answer ? QString()
                       : QStringLiteral("the layout was not saved: %1")
                             .arg(answer.error().Message);
            if (!answer) {
                qWarning("roost: the layout was not saved: %s",
                         qPrintable(answer.error().Message));
            }
            if (problem != WriteProblem_) {
                WriteProblem_ = problem;
                UpdateProblems();
            }
        });
}

LayoutKnownPanels Shell::KnownPanels() const {
    LayoutKnownPanels known;
    for (const QString& type : Loader_.PanelTypes()) {
        if (const auto info = Loader_.Info(type)) {
            known.insert(type,
                         {.Inputs = info->Inputs, .Outputs = info->Outputs});
        }
    }
    return known;
}

void Shell::PublishPanelTypes() {
    if (!LayoutMode_) {
        return;
    }
    const QStringList types = Loader_.PanelTypes();
    if (Root_.property("panelTypes").toStringList() != types) {
        Root_.setProperty("panelTypes", types);
    }
    QStringList withInputs;
    for (const QString& type : types) {
        if (const auto info = Loader_.Info(type);
            info && !info->Inputs.isEmpty()) {
            withInputs.append(type);
        }
    }
    if (Root_.property("panelsWithInputs").toStringList() != withInputs) {
        Root_.setProperty("panelsWithInputs", withInputs);
    }
}

void Shell::OnLayoutEdit(const QString& action, const QVariant& args) {
    const QVariantMap map = AsMap(args);
    const QString instance = map.value(QStringLiteral("instance")).toString();
    if (!LayoutSettled_) {
        EditProblem_ = QStringLiteral("the layout is not loaded yet");
        UpdateProblems();
        return;
    }
    if (instance.isEmpty()) {
        EditProblem_ = QStringLiteral("no panel to edit");
        UpdateProblems();
        return;
    }
    const LayoutKnownPanels known = KnownPanels();
    const auto text = [&map](const char* key) {
        return map.value(QLatin1String(key)).toString();
    };
    LayoutEdit edit;
    if (action == QLatin1String("editLinks")) {
        if (FindLeaf(Layout_.Root, instance) == nullptr) {
            EditProblem_ = QStringLiteral("there is no panel %1").arg(instance);
            UpdateProblems();
            return;
        }
        // A panel with no inputs has no links to edit: nothing to open.
        if (LinkChoices(Layout_, instance, known).empty()) {
            return;
        }
        EditProblem_.clear();
        UpdateProblems();
        OpenLinkEditor(instance);
        return;
    }
    if (action == QLatin1String("splitRight") ||
        action == QLatin1String("splitDown")) {
        edit =
            SplitPanel(Layout_, instance, action == QLatin1String("splitRight"),
                       text("panel"), known);
    } else if (action == QLatin1String("intoTabs")) {
        edit = MoveIntoTabs(Layout_, instance);
    } else if (action == QLatin1String("outOfTabs")) {
        edit = MoveOutOfTabs(Layout_, instance);
    } else if (action == QLatin1String("close")) {
        edit = ClosePanel(Layout_, instance);
    } else if (action == QLatin1String("setLink")) {
        edit = text("from").isEmpty()
                   ? RemoveLink(Layout_, instance, text("input"))
                   : SetLink(Layout_, instance, text("input"), text("from"),
                             text("output"), known);
    } else {
        EditProblem_ = QStringLiteral("unknown layout edit %1").arg(action);
        UpdateProblems();
        return;
    }
    EditProblem_ = edit.Refused;
    if (edit.Refused.isEmpty() && edit.Changed) {
        ApplyLayout(edit.Tree);
        SaveLayout();
    }
    UpdateProblems();
    if (action == QLatin1String("setLink")) {
        OpenLinkEditor(instance);
    }
    if (!edit.Focus.isEmpty() && edit.Refused.isEmpty() &&
        Root_.metaObject()->indexOfMethod("focusInstance(QVariant)") >= 0) {
        QMetaObject::invokeMethod(&Root_, "focusInstance",
                                  Q_ARG(QVariant, edit.Focus));
    }
}

void Shell::OpenLinkEditor(const QString& instance) {
    const LayoutNode* leaf = FindLeaf(Layout_.Root, instance);
    if (leaf == nullptr ||
        Root_.metaObject()->indexOfMethod("openLinkEditor(QVariant)") < 0) {
        return;
    }
    QVariantList inputs;
    for (const LayoutInputChoices& input :
         LinkChoices(Layout_, instance, KnownPanels())) {
        QVariantList choices;
        for (const LayoutLinkChoice& choice : input.Choices) {
            choices.append(QVariantMap{
                {"from", choice.From},
                {"output", choice.Output},
                {"label",
                 QStringLiteral("%1.%2").arg(choice.From, choice.Output)}});
        }
        inputs.append(QVariantMap{
            {"input", input.Input},
            {"type", input.Type},
            {"from", input.Current ? input.Current->From : QString()},
            {"output", input.Current ? input.Current->Output : QString()},
            {"problem", input.Current ? input.Current->Problem : QString()},
            {"choices", choices}});
    }
    const QVariant state = QVariantMap{
        {"instance", instance}, {"panel", leaf->Panel}, {"inputs", inputs}};
    QMetaObject::invokeMethod(&Root_, "openLinkEditor", Q_ARG(QVariant, state));
}

void Shell::OnRefused(const rpc::Error& error) {
    const QJsonObject data = error.Data.toObject();
    QString message = error.Message;
    if (!data.contains("roostd") &&
        (data.contains("protocol") || data.contains("ghosttyVt"))) {
        message += QStringLiteral(
                       " (roostd: protocol %1, libghostty-vt %2; this window: "
                       "protocol %3, libghostty-vt %4)")
                       // One arg() call: a "%N" inside a string from roostd is
                       // not substituted by a later one.
                       .arg(data.value("protocol").toVariant().toString(),
                            data.value("ghosttyVt").toString(),
                            QString::number(protocol::VERSION),
                            QString::fromStdString(GhosttyVtVersion()));
    }
    Mismatch_ =
        error.Code == static_cast<int>(protocol::EErrorCode::VersionMismatch);
    // A roostd of this protocol that is not of this build: the client wrote
    // `roostd` (its version, or null) into the data, and its message already
    // names both versions and says to restart.
    const bool otherBuild = Mismatch_ && data.contains("roostd");
    if (Mismatch_ && Launcher_ != nullptr) {
        message += QStringLiteral(
            "\nRestarting closes the warm terminals; Claude sessions keep "
            "running.");
    } else if (otherBuild) {
        message += QStringLiteral(". Then press Retry.");
    } else {
        message += QStringLiteral(". Restart roostd, then press Retry.");
    }
    SetConnection(QStringLiteral("refused"), message);
}

void Shell::OnRetry() {
    SetConnection(QStringLiteral("connecting"), {});
    Api_.Connect(SocketPath_);
}

void Shell::OnRestartRequested() {
    // The button is there only for this state; a stray signal does nothing.
    if (Launcher_ == nullptr || Connection_ != QLatin1String("refused") ||
        !Mismatch_) {
        return;
    }
    Launcher_->Restart(SocketPath_, Api_.RefusedPeerPid());
}

bool Shell::IsWaiting() const {
    return Connection_ == QLatin1String("connecting") ||
           Connection_ == QLatin1String("disconnected");
}

void Shell::SetConnection(const QString& state, const QString& message) {
    Connection_ = state;
    ConnectionMessage_ = message;
    QString shown = state;
    QString shownMessage = message;
    bool offerRestart = false;
    if (Launcher_ != nullptr && state == QLatin1String("refused")) {
        if (Launcher_->Restarting()) {
            shown = QStringLiteral("restarting");
        } else {
            offerRestart = Mismatch_;
            if (!Launcher_->Failure().isEmpty()) {
                shownMessage =
                    Launcher_->Failure() + QLatin1Char('\n') + message;
            }
        }
    } else if (Launcher_ != nullptr && IsWaiting()) {
        if (!Launcher_->Failure().isEmpty()) {
            shown = QStringLiteral("startFailed");
            shownMessage = Launcher_->Failure();
        } else if (Launcher_->Starting()) {
            shown = QStringLiteral("starting");
        }
    }
    Root_.setProperty("connection", shown);
    Root_.setProperty("connectionMessage", shownMessage);
    Root_.setProperty("restartOffered", offerRestart);
    // The placeholders of the empty slots tell the state of the connection.
    for (const Slot& slot : Slots_) {
        if (slot.Item && slot.Panel.isNull()) {
            Write(slot, "error", ErrorFor(slot.Type));
        }
    }
}

void Shell::UpdateProblems() {
    QStringList problems;
    const auto listed = Api_.Plugins().Plugins().value("problems").toArray();
    for (const auto& problem : listed) {
        problems.append(problem.toString());
    }
    for (const auto& problem : Loader_.Problems()) {
        problems.append(
            problem.Panel.isEmpty()
                ? QStringLiteral("plugin %1: %2")
                      .arg(problem.Plugin, problem.Message)
                : QStringLiteral("plugin %1, panel %2: %3")
                      .arg(problem.Plugin, problem.Panel, problem.Message));
    }
    for (const Slot& slot : Slots_) {
        problems.append(slot.LinkProblems);
    }
    problems.append(LayoutProblems_);
    if (!EditProblem_.isEmpty()) {
        problems.append(EditProblem_);
    }
    if (!WriteProblem_.isEmpty()) {
        problems.append(WriteProblem_);
    }
    if (Root_.property("problems").toStringList() != problems) {
        Root_.setProperty("problems", problems);
    }
}

void Shell::Write(const Slot& slot, const char* name, const QVariant& value) {
    if (slot.Item) {
        slot.Item->setProperty(name, value);
    }
}

}  // namespace roost
