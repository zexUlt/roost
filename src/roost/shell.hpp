#pragma once

#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>

#include "api/api.hpp"
#include "roost/daemon_launcher.hpp"
#include "roost/layout_model.hpp"
#include "roost/plugin_loader.hpp"

namespace roost {

// The controller of the window's layout (design.md §6): the QML root
// (Main.qml) shows the tree that the shell publishes in `layoutTree`; the
// layout view builds a PanelSlot, objectName "panelSlot", for each leaf and
// tells the shell (`slotCreated`); the shell asks the loader for a panel of
// the leaf's type and instance id, hands it to the slot, works out the links
// of the leaf, and writes the state of the connection and the problems to the
// root. Plain C++, made from main() like SingleSession; GUI thread only.
//
// The layout: the window reads the `layout` document of the store once, when
// the connection is up and the plugins are listed, and shows the default
// layout until then (its slots wait with a placeholder; no panel is created
// before the layout is known). It writes by patch: when the user finishes
// resizing a split (`splitResized`), when the user chooses a tab
// (`tabChosen`), after each edit of the layout
// (`layoutEdit`: split, tabs, close, links, hide or show the lists;
// src/roost/layout_commands.hpp),
// and in SetLayout. A failed write is a problem of the window. A change of the
// document by another writer is not followed while the window runs.
//
// A root that has no `layoutTree` (a window of a test with its own slots) is
// served the old way: the slots found at the start, the panel types as the
// instance ids "list" and "terminal", one link from a sessions.list to a
// terminal.view.
//
// The root takes (written by the shell): `connection` ("connecting",
// "connected", "disconnected", "refused", "starting" while the launcher has a
// roostd coming up and "startFailed" (the message is why) after a start that
// failed, both only while the window is not connected, and "restarting" while
// the launcher waits for the old roostd of a refusal to go),
// `connectionMessage`, `restartOffered` (the refusal is VersionMismatch, from
// roostd or for a roostd of another version than the window's, and there is a
// launcher), `problems` (a list of strings) and `layoutTree`; it gives the
// signals `retryRequested()`, `restartRequested()` (the launcher replaces the
// roostd, nothing else of the window moves), `slotCreated(slot)` and
// `splitResized(path, sizes)`. A slot takes `panel`, `error`, `links`,
// `linkInput`, `linkOutput`. A root with `layoutTree` also takes
// `listPanelTypes`, the panel types that are lists of sessions (the keyboard
// command to the list looks for one).
//
// A link of a leaf names its input, the leaf it comes from and the output
// there. It works when both panels exist, the types of the ports match and
// both roots have the properties. An output the source panel type does not
// declare falls back to its only output of the input's type; none or several
// is a problem in the strip, and the link stays off. A declared name the
// panel's root has no property for is the loader's problem and also leaves the
// link off.
//
// It must be destroyed before the loader and the engine: its destructor gives
// the panels back to the loader.
class Shell : public QObject {
    Q_OBJECT

public:
    // `root` is the QML root (the slots are its descendants), `loader`,
    // `api` and `launcher` (none with --no-spawn) are used and not owned.
    Shell(QObject& root, PluginLoader& loader, api::Api& api,
          QString socketPath, DaemonLauncher* launcher = nullptr);
    ~Shell() override;

    // The layout that is shown.
    const Layout& CurrentLayout() const { return Layout_; }

    // Shows another layout and saves it (store.patch of the `layout`
    // document): the panels of the leaves that stay are kept as they are, those
    // of the leaves that go are given back to the loader, new leaves get
    // panels. Nothing is saved while the stored layout is not read, if it was
    // not readable or if it is of a version this roost does not know. For the
    // tests; the edits of the user go through `layoutEdit`.
    void SetLayout(const Layout& layout);

    // Whether the window shows what `roost --smoke-sessions` checks. `Ready`:
    // connected, the layout read, every panel of it created, no problem of the
    // loader or of a link, sessions.list answered with at least one session,
    // and, if a list opens a session at start, that session is open in the
    // terminal that shows it (roostd answered session.open, so its attach
    // process is started, or was already running for a warm session) or has
    // ended.
    // The window opens one when the list panel selects a session once it has
    // restored its view; the list panel tells that by its optional property
    // `restored` (true once it has decided; a panel without it is not waited
    // for), the terminal by the `status` of its TerminalItem "terminal" (a
    // terminal panel without one is not waited for). `Failed`: it cannot
    // become ready by waiting (a problem of the loader or of a link, the
    // connection refused, roostd refused to open the session). `Reason` names
    // what is missing; empty when Ready.
    struct Readiness {
        bool Ready = false;
        bool Failed = false;
        QString Reason;
    };
    Readiness CheckReadiness() const;

private slots:
    // The root's retryRequested(), connected by name: the signal is a QML one.
    void OnRetry();
    // The root's restartRequested(): the Restart roostd button.
    void OnRestartRequested();
    // The root's slotCreated(slot): the layout view built a slot.
    void OnSlotCreated(const QVariant& slot);
    // The root's splitResized(path, sizes).
    void OnSplitResized(const QVariant& path, const QVariant& sizes);
    // The root's tabChosen(path, index): the user clicked a tab of a stack.
    void OnTabChosen(const QVariant& path, int index);
    // The root's layoutEdit(action, args): an edit of the layout from a
    // command or the panel menu (layout_commands.hpp), or the request to show
    // the link editor.
    void OnLayoutEdit(const QString& action, const QVariant& args);

private:
    struct Slot {
        QPointer<QQuickItem> Item;
        QString Type;
        // The id of the leaf; empty for a slot of a window without a layout
        // view.
        QString Instance;
        QPointer<QQuickItem> Panel;
        // Why the links of this panel are off, if a declaration is wrong.
        QStringList LinkProblems;
        // What the slot was last told about its links.
        QVariantList Links;
        QString LinkInput;
        QString LinkOutput;
    };

    // The slot of the leaf, or null.
    Slot* SlotOf(const QString& instance);
    // Whether the panel is a list of sessions (output `selected` of type
    // SessionRef) and whether it takes a session (a link into an input of
    // type SessionRef), by port and not by id.
    bool IsListSlot(const Slot& slot) const;
    bool TakesSession(const Slot& slot) const;
    // Tells the root which panel types are lists: `listPanelTypes`.
    void UpdateListTypes();
    void OnPanelTypesChanged();
    void OnPanelAboutToBeDestroyed(QQuickItem* panel);
    void OnRefused(const rpc::Error& error);
    // Asks the store for the layout, once.
    void ReadLayout();
    // Shows the stored layout once it is read and the plugins are listed.
    void TrySettle();
    void ApplyLayout(Layout layout);
    void PublishLayout();
    void SaveLayout();
    // The panel types that can be created, with their ports.
    LayoutKnownPanels KnownPanels() const;
    // Writes the root's `panelTypes`.
    void PublishPanelTypes();
    // Gives the root the inputs of the leaf with what each can be fed from
    // (`openLinkEditor`).
    void OpenLinkEditor(const QString& instance);
    // Works out the links of the leaves from the panels there are now.
    void UpdateLinks();
    // Whether the connection is being waited for (the banner may say that
    // roostd is being started, or that its start failed).
    bool IsWaiting() const;
    // Fills every empty slot; safe to call from a slot of the loader's signals
    // that CreatePanel itself emits.
    void Refill();
    void Fill(Slot& slot);
    QString ErrorFor(const QString& type) const;
    // The link of a slot of a window without a layout view.
    void SetLegacyLink(Slot& slot, const PluginLoader::PanelInfo& info);
    void SetConnection(const QString& state, const QString& message);
    void UpdateProblems();
    void Write(const Slot& slot, const char* name, const QVariant& value);

    QObject& Root_;
    PluginLoader& Loader_;
    api::Api& Api_;
    QString SocketPath_;
    DaemonLauncher* Launcher_;
    QList<Slot> Slots_;
    QString Connection_;
    QString ConnectionMessage_;
    // The last refusal was a VersionMismatch (another protocol or library, or
    // another version of roostd).
    bool Mismatch_ = false;
    bool Filling_ = false;
    bool FillAgain_ = false;
    // The root shows a layout (it has `layoutTree`).
    bool LayoutMode_ = false;
    Layout Layout_;
    bool LayoutPublished_ = false;
    // The panel types the published tree has collapsed.
    QSet<QString> HiddenPublished_;
    bool LayoutRequested_ = false;
    bool LayoutAnswered_ = false;
    bool LayoutSettled_ = false;
    // False when the stored layout could not be read or is of another
    // version: it is not overwritten.
    bool LayoutWritable_ = true;
    QJsonValue LayoutDocument_;
    QStringList LayoutProblems_;
    // The last edit that was refused (the reason), and the last write of the
    // layout that failed; the strip shows them until the next edit that works
    // or the next write that does.
    QString EditProblem_;
    QString WriteProblem_;
};

}  // namespace roost
