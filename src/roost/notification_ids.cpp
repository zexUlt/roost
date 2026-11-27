#include "roost/notification_ids.hpp"

namespace roost {

namespace {

const QString PREFIX = QStringLiteral("roost-");

QString Head(const QString& token) { return PREFIX + token + QLatin1Char('-'); }

}  // namespace

QString NotificationIdentifier(const QString& token, const QString& id) {
    return Head(token) + id;
}

std::optional<QString> ActivatedNotificationId(const QString& identifier,
                                               const QString& userInfoId,
                                               const QString& token) {
    // The id may hold a dash; the head is matched whole, so a token that is a
    // prefix of another one does not match.
    const QString head = Head(token);
    if (token.isEmpty() || !identifier.startsWith(head)) {
        return std::nullopt;
    }
    const QString id = identifier.mid(head.size());
    if (id.isEmpty() || id != userInfoId) {
        return std::nullopt;
    }
    return id;
}

}  // namespace roost
