#pragma once

#include <QDBusConnection>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtTypes>

#include <functional>

#include "roost/desktop_notifier.hpp"

namespace roost {

// The icon of the notification: the absolute path of the 128 px PNG of the
// install layout (cmake/Install.cmake: bin/ next to share/icons/hicolor), if
// it is there, else the name of the icon in the theme. `applicationDir` is the
// directory of the executable.
QString NotificationIcon(const QString& applicationDir);

// The arguments of org.freedesktop.Notifications.Notify, in order: app_name,
// replaces_id (0: a new one), app_icon, summary, body, actions (the default
// one, which a click on the notification invokes), hints and expire_timeout
// (-1: the server's own).
// https://specifications.freedesktop.org/notification/latest/protocol.html
QVariantList NotifyArguments(const QString& title, const QString& body,
                             const QString& icon);

// Notifications over D-Bus on the session bus
// (org.freedesktop.Notifications). The bus is asked for at the first Show, not
// at the start of the window: a window that never shows one does not touch it.
// With no session bus or no notification service the first Show warns, once,
// and nothing is shown, here or after.
//
// A click on the notification (the action "default") is Activated with the id
// that Show was given. A closed notification is forgotten (NotificationClosed).
class LinuxDesktopNotifier : public DesktopNotifier {
    Q_OBJECT

public:
    using Connect = std::function<QDBusConnection()>;

    // `connect` gives the bus; the session bus by default.
    explicit LinuxDesktopNotifier(Connect connect = &SessionBus);

    void Show(const QString& id, const QString& title,
              const QString& body) override;

    // The notifications that are shown and not closed yet.
    qsizetype Open() const { return static_cast<qsizetype>(Ids_.size()); }

private slots:
    void OnActionInvoked(uint dbusId, const QString& action);
    void OnClosed(uint dbusId, uint reason);

private:
    static QDBusConnection SessionBus();
    // Connects to the bus and the signals once; false when it cannot.
    bool EnsureReady();
    void Warn(const QString& message);

    Connect Connect_;
    QDBusConnection Bus_;
    bool Tried_ = false;
    bool Ready_ = false;
    bool Warned_ = false;
    // The ids of the server -> ours.
    QHash<uint, QString> Ids_;
};

}  // namespace roost
