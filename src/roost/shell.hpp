#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QStringList>

#include "api/api.hpp"
#include "roost/daemon_launcher.hpp"
#include "roost/plugin_loader.hpp"

namespace roost {

// The controller of the window's fixed layout (design.md §6, until M6): the
// QML root (Main.qml) has PanelSlots, objectName "panelSlot", with a
// panelType each; the shell asks the loader for a panel of that type, hands
// it to the slot, works out the one link between the list and the terminal,
// and writes the state of the connection and the problems to the root. Plain
// C++, made from main() like SingleSession; GUI thread only.
//
// The root takes (written by the shell): `connection` ("connecting",
// "connected", "disconnected", "refused", "starting" while the launcher has a
// roostd coming up and "startFailed" (the message is why) after a start that
// failed, both only while the window is not connected, and "restarting" while
// the launcher waits for the old roostd of a refusal to go),
// `connectionMessage`, `restartOffered` (the refusal is VersionMismatch and
// there is a launcher), `problems` (a list of strings); it gives the signals
// `retryRequested()` and `restartRequested()` (the launcher replaces the
// roostd, nothing else of the window moves). A slot takes `panel`, `error`,
// `linkInput`, `linkOutput`.
//
// The link: the sessions.list panel must declare one output of type
// SessionRef, the terminal.view panel one input of type SessionRef. None or
// several is a problem in the strip, and the link stays off (the names stay
// empty). A declared name the panel's root has no property for is the
// loader's problem and also leaves the link off.
//
// It must be destroyed before the loader and the engine: its destructor gives
// the panels back to the loader.
class Shell : public QObject {
    Q_OBJECT

public:
    static constexpr const char* LIST_PANEL = "sessions.list";
    static constexpr const char* TERMINAL_PANEL = "terminal.view";
    static constexpr const char* SESSION_REF = "SessionRef";

    // `root` is the QML root (the slots are its descendants), `loader`,
    // `api` and `launcher` (none with --no-spawn) are used and not owned.
    Shell(QObject& root, PluginLoader& loader, api::Api& api,
          QString socketPath, DaemonLauncher* launcher = nullptr);
    ~Shell() override;

    // Whether the window shows what `roost --smoke-sessions` checks. `Ready`:
    // connected, both panels created, no problem of the loader or of a link,
    // sessions.list answered with at least one session, and, if the window
    // opens a session at start, that session is open in the terminal (roostd
    // answered session.open, so its attach process is started, or was already
    // running for a warm session) or has ended.
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

private:
    struct Slot {
        QPointer<QQuickItem> Item;
        QString Type;
        QPointer<QQuickItem> Panel;
        // Why the link of this panel is off, if the declaration is wrong.
        QString LinkProblem;
    };

    // The panel made for the slot of the type, or null.
    const QQuickItem* PanelOf(const char* type) const;
    void OnPanelTypesChanged();
    void OnPanelAboutToBeDestroyed(QQuickItem* panel);
    void OnRefused(const rpc::Error& error);
    // Whether the connection is being waited for (the banner may say that
    // roostd is being started, or that its start failed).
    bool IsWaiting() const;
    // Fills every empty slot; safe to call from a slot of the loader's signals
    // that CreatePanel itself emits.
    void Refill();
    void Fill(Slot& slot);
    QString ErrorFor(const QString& type) const;
    void SetLink(Slot& slot, const PluginLoader::PanelInfo& info);
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
    // The last refusal was a VersionMismatch.
    bool Mismatch_ = false;
    bool Filling_ = false;
    bool FillAgain_ = false;
};

}  // namespace roost
