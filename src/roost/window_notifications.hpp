#pragma once

#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QWindow>

#include <map>
#include <optional>

#include "api/api.hpp"
#include "api/bus_service.hpp"
#include "roost/desktop_notifier.hpp"

namespace roost {

// The desktop notifications of the window (docs/design/05-plugins.md): the
// window shows what the backend of the notifications plugin asks for on the
// bus, `notifications.show` {title, body, agent?, session?}, if it is the
// window that was focused last; a click on the notification raises the window
// and opens the session.
//
// Which window: each roost window is a process of its own and knows nothing
// of the others but the bus. When the window gets the keyboard focus it
// publishes `shell.windowFocused` {window}, `window` being an id made once for
// the process; every window remembers the last `window` it heard of in that
// event, its own included, since roostd gives all the windows the events in one
// order. A window shows a notification only if the last one it heard of is its
// own. A window that has heard of none shows nothing: with no window ever
// focused there is none to choose, and a window that is only started and not
// focused is not where the user looks. Nothing is published when a window
// closes; the others go on with the last id they have, so a notification
// asked for after the last focused window has gone is shown by nobody.
//
// The signal is QGuiApplication::focusWindowChanged to this window and not
// applicationStateChanged: it says which window has the focus, and the
// application state does not say that a window of it has one; it is also
// what a test can drive on the platform "offscreen" with requestActivate.
// Only the main window counts; the focus in the settings window or the about
// window does not.
//
// Not shown either: a notification of the session that a terminal panel of the
// window shows, when the window has the focus (the user sees it already). Any
// other case is shown as above, the window being out of focus or the session
// another.
//
// A click: the window is raised and activated; if the notification names a
// session, the command `sessions.open` {agent, id} runs in the window (the
// sessions panel handles it, and selects the session as Enter in its list
// does). A notification without one (the test of the plugin) only raises.
//
// When the connection to roostd is lost the last window heard of is forgotten
// (events were missed), and a window that is focused, or was the last focused
// when the connection was lost, says so again when the connection is back.
//
// GUI thread only. The api, the notifier and the window must outlive it.
class WindowNotifications : public QObject {
    Q_OBJECT

public:
    WindowNotifications(api::Api& api, DesktopNotifier& notifier,
                        QWindow& window, QObject* parent = nullptr);

    // The id of this window in shell.windowFocused.
    const QString& WindowId() const { return WindowId_; }

    // The last window that said it was focused; empty before any did.
    const QString& LastFocusedWindow() const { return LastFocused_; }

    // Whether this window shows the next notification.
    bool IsLastFocused() const { return LastFocused_ == WindowId_; }

private:
    struct Target {
        QString Agent;
        QString Session;
    };

    void OnFocusWindowChanged(QWindow* focused);
    void PublishFocus();
    // Whether a terminal of the window shows the session now.
    bool IsShown(const QString& agent, const QString& session) const;
    void OnShow(const QJsonValue& payload);
    void OnActivated(const QString& id);
    void Raise();
    void OpenSession(const Target& target);

    api::Api& Api_;
    DesktopNotifier& Notifier_;
    QPointer<QWindow> Window_;
    QString WindowId_;
    QString LastFocused_;
    bool Focused_ = false;
    // This window was the last focused when the connection was lost: it says
    // so again when the connection is back, though it has no focus now.
    bool WasLast_ = false;
    quint64 NextId_ = 1;
    // What a click on a notification that names a session opens, by the id
    // given to the notifier; the oldest go first once there are too many.
    std::map<quint64, Target> Targets_;
};

}  // namespace roost
