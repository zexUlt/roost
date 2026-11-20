#include "claude/backend/agents.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSet>

#include <optional>

#include "common/json_rpc.hpp"

namespace roost::claude {

namespace {

QString StateOf(const QJsonObject& entry, const QString& id,
                QStringList& warnings) {
    const QString state = entry.value(QStringLiteral("state")).toString();
    if (state == QStringLiteral("working") ||
        state == QStringLiteral("blocked") || state == QStringLiteral("idle") ||
        state == QStringLiteral("failed")) {
        return state;
    }
    if (state == QStringLiteral("needs_input")) {
        return QStringLiteral("blocked");
    }
    if (state == QStringLiteral("done")) {
        // A finished session that took a new turn.
        const bool busy = entry.value(QStringLiteral("status")).toString() ==
                          QStringLiteral("busy");
        return busy ? QStringLiteral("working") : state;
    }
    warnings.append(
        QStringLiteral("session %1 is in state \"%2\", taken as idle")
            .arg(id, state));
    return QStringLiteral("idle");
}

}  // namespace

QJsonObject ToJson(const Session& session) {
    QJsonObject json{
        {QStringLiteral("id"), session.Id},
        {QStringLiteral("state"), session.State},
        {QStringLiteral("title"), session.Title},
        {QStringLiteral("cwd"), session.WorkingDirectory},
    };
    if (session.StartedAt > 0) {
        json.insert(QStringLiteral("startedAt"), session.StartedAt);
    }
    return json;
}

std::expected<Listing, QString> ParseAgents(QByteArrayView output) {
    const QJsonDocument document =
        QJsonDocument::fromJson(output.toByteArray());
    if (!document.isArray()) {
        return std::unexpected(
            QStringLiteral("claude agents printed no JSON array"));
    }
    Listing listing;
    QSet<QString> seen;
    for (const QJsonValue& value : document.array()) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("kind")).toString() !=
            QStringLiteral("background")) {
            continue;
        }
        const QJsonValue id = entry.value(QStringLiteral("id"));
        if (!id.isString() || id.toString().isEmpty()) {
            listing.Warnings.append(
                QStringLiteral("a background session without an id is left "
                               "out"));
            continue;
        }
        Session session;
        session.Id = id.toString();
        if (seen.contains(session.Id)) {
            listing.Warnings.append(
                QStringLiteral("session %1 is listed twice; the first one is "
                               "kept")
                    .arg(session.Id));
            continue;
        }
        seen.insert(session.Id);
        session.State = StateOf(entry, session.Id, listing.Warnings);
        const QString name = entry.value(QStringLiteral("name")).toString();
        session.Title = name.isEmpty() ? session.Id : name;
        session.WorkingDirectory =
            entry.value(QStringLiteral("cwd")).toString();
        const std::optional<qint64> startedAt = plugin::jsonrpc::ToInteger(
            entry.value(QStringLiteral("startedAt")));
        session.StartedAt = startedAt.value_or(0) > 0 ? *startedAt : 0;
        listing.Sessions.push_back(session);
    }
    return listing;
}

}  // namespace roost::claude
