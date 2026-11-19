#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QStringList>

#include "api/api.hpp"
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
// "connected", "disconnected", "refused"), `connectionMessage`, `problems`
// (a list of strings); it gives the signal `retryRequested()`. A slot takes
// `panel`, `error`, `linkInput`, `linkOutput`.
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

    // `root` is the QML root (the slots are its descendants), `loader` and
    // `api` are used and not owned.
    Shell(QObject& root, PluginLoader& loader, api::Api& api,
          QString socketPath);
    ~Shell() override;

private slots:
    // The root's retryRequested(), connected by name: the signal is a QML one.
    void OnRetry();

private:
    struct Slot {
        QPointer<QQuickItem> Item;
        QString Type;
        QPointer<QQuickItem> Panel;
        // Why the link of this panel is off, if the declaration is wrong.
        QString LinkProblem;
    };

    void OnPanelTypesChanged();
    void OnPanelAboutToBeDestroyed(QQuickItem* panel);
    void OnRefused(const rpc::Error& error);
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
    QList<Slot> Slots_;
    QString Connection_;
    bool Filling_ = false;
    bool FillAgain_ = false;
};

}  // namespace roost
