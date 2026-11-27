#pragma once

#include <QString>

#include <optional>

namespace roost {

// The identifier of a request of the macOS notification center for the
// notification `id` of the window, issued by the process whose random `token`
// is given: "roost-<token>-<id>". The token (a fresh one per process) keeps
// what an earlier run, or another roost, left in the Notification Center from
// being taken for ours. Plain functions, so that they are built and tested on
// every platform.
QString NotificationIdentifier(const QString& token, const QString& id);

// The window's id of the notification a click on `identifier` (whose userInfo
// named `userInfoId`) is about: only when the identifier is ours (issued with
// `token`) and agrees with the userInfo. A notification of an earlier run, of
// another process or of another kind gives nothing.
std::optional<QString> ActivatedNotificationId(const QString& identifier,
                                               const QString& userInfoId,
                                               const QString& token);

}  // namespace roost
