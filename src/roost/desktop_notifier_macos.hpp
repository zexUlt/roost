#pragma once

#include <QString>
#include <QtTypes>

#include <memory>
#include <optional>

#include "roost/desktop_notifier.hpp"

namespace roost {

// The identifier of a request of UNUserNotificationCenter for the notification
// `id` of the window, issued by the process `pid`: "roost-<pid>-<id>". The pid
// keeps what an earlier run left in the Notification Center from being taken
// for ours.
QString NotificationIdentifier(qint64 pid, const QString& id);

// The window's id of the notification a click on `identifier` (whose userInfo
// named `userInfoId`) is about: only when the identifier is ours (issued by
// `pid`) and agrees with the userInfo. A notification of an earlier run, of
// another process or of another kind gives nothing.
std::optional<QString> ActivatedNotificationId(const QString& identifier,
                                               const QString& userInfoId,
                                               qint64 pid);

// Notifications of macOS: UNUserNotificationCenter, with the icon of the
// bundle (roost.icns). It works only inside roost.app: UNUserNotificationCenter
// raises an exception in a process with no bundle (build/<preset>/bin/roost),
// so there the first Show warns, once, and nothing is shown, here or after.
//
// The permission is asked for at the first Show, not at the start of the
// window; that Show waits for the answer (the others queue behind it), the GUI
// thread never does. Denied, or an error: one warning, and nothing is shown
// until the next start. A notification has no sound.
//
// A click on the notification (the default action) is Activated with the id
// that Show was given, on the GUI thread, and only for a notification this
// process issued. A notification stays in the Notification Center when roost
// quits; a click on it then launches nothing.
class MacDesktopNotifier : public DesktopNotifier {
    Q_OBJECT

public:
    MacDesktopNotifier();
    ~MacDesktopNotifier() override;

    void Show(const QString& id, const QString& title,
              const QString& body) override;

private:
    struct Impl;
    std::unique_ptr<Impl> Impl_;
};

}  // namespace roost
