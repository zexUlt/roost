#pragma once

#include <QString>

#include <memory>

#include "roost/desktop_notifier.hpp"

namespace roost {

// Notifications of macOS: UNUserNotificationCenter, with the icon of the
// bundle (roost.icns). It works only inside roost.app: UNUserNotificationCenter
// raises an exception in a process with no bundle (build/<preset>/bin/roost),
// so there the first Show warns, once, and nothing is shown, here or after.
//
// The permission (alerts only: a notification has no sound) is asked for at
// the first Show, not at the start of the window; that Show waits for the
// answer, the GUI thread never does. The Shows that come meanwhile queue behind
// it, the last three only. Denied, or an error: one warning, and nothing is
// shown until the next start.
//
// A click on the notification (the default action) is Activated with the id
// that Show was given, on the GUI thread, and only for a notification this
// process issued (NotificationIdentifier). The destructor takes this process's
// notifications out of the Notification Center, so a click after roost quit
// has nothing to hit. One of a roost that crashed may stay; a click on it
// launches roost and does not open the session.
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
