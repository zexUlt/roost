#include "roost/desktop_notifier_macos.hpp"

namespace roost {

namespace {

const QString PREFIX = QStringLiteral("roost-");

}  // namespace

QString NotificationIdentifier(qint64 pid, const QString& id) {
    return PREFIX + QString::number(pid) + QLatin1Char('-') + id;
}

std::optional<QString> ActivatedNotificationId(const QString& identifier,
                                               const QString& userInfoId,
                                               qint64 pid) {
    // The id may hold a dash, the pid is only digits: the first dash after the
    // prefix ends the pid.
    const QString head = PREFIX + QString::number(pid) + QLatin1Char('-');
    if (!identifier.startsWith(head)) {
        return std::nullopt;
    }
    const QString id = identifier.mid(head.size());
    if (id.isEmpty() || id != userInfoId) {
        return std::nullopt;
    }
    return id;
}

}  // namespace roost
