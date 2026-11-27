#pragma once

#include <QObject>
#include <QString>

#include <memory>

namespace roost {

// What the window needs of the operating system to show a notification: the
// seam between WindowNotifications, which decides whether and what to show,
// and the platform, which draws it. One implementation per platform is built
// (CMake picks MakeDesktopNotifier's file): D-Bus on Linux, UserNotifications
// on macOS, nothing elsewhere. Tests install their own through the same
// interface.
//
// GUI thread only. `id` is the window's own name of a notification: the
// implementation gives it back in Activated and does not interpret it.
class DesktopNotifier : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // Shows a notification. When the system cannot show it, the implementation
    // warns (once, not for each notification) and shows nothing.
    virtual void Show(const QString& id, const QString& title,
                      const QString& body) = 0;

signals:
    // The user clicked the notification `id` (its default action).
    void Activated(const QString& id);
};

// The notifier of this platform.
std::unique_ptr<DesktopNotifier> MakeDesktopNotifier();

}  // namespace roost
