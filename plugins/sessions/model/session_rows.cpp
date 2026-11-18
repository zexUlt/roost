#include "sessions/model/session_rows.hpp"

#include <QRegularExpression>

#include <algorithm>
#include <map>
#include <tuple>

namespace roost::sessions {

namespace {

const QString DONE = QStringLiteral("done");
const QString WORKING = QStringLiteral("working");
const QString UNCATEGORIZED = QStringLiteral("uncategorized");

QString TitleOf(const SessionEntry& session) {
    return session.Name.isEmpty() ? session.AgentTitle : session.Name;
}

bool Matches(const SessionEntry& session, const QStringList& words) {
    const QString haystack =
        (TitleOf(session) + QLatin1Char(' ') + session.State +
         QLatin1Char(' ') +
         (session.Category.isEmpty() ? UNCATEGORIZED : session.Category))
            .toLower();
    return std::ranges::all_of(
        words, [&](const QString& word) { return haystack.contains(word); });
}

Row SessionRow(const SessionEntry& session, const QString& key,
               const QString& opened) {
    Row row;
    row.Kind = ERowKind::Session;
    row.Key = key;
    row.Category = session.Category;
    row.Agent = session.Agent;
    row.Id = session.Id;
    row.Title = TitleOf(session);
    row.AgentTitle = session.AgentTitle;
    row.State = session.State;
    row.Cwd = session.Cwd;
    row.StartedAt = session.StartedAt;
    row.Unread = session.Unread;
    row.IsOpen = key == opened;
    row.Archived = session.Archived;
    return row;
}

struct Group {
    std::vector<const SessionEntry*> Members;
    int Unread = 0;
    int Ranked = 0;
};

}  // namespace

QString SessionKey(const QString& agent, const QString& id) {
    return agent + QLatin1Char('/') + id;
}

RowsResult BuildRows(const std::vector<SessionEntry>& sessions,
                     const ViewState& view) {
    RowsResult result;
    const QStringList words = view.Query.toLower().split(
        QRegularExpression(QStringLiteral("\\s+"),
                           QRegularExpression::UseUnicodePropertiesOption),
        Qt::SkipEmptyParts);

    std::map<QString, Group> groups;
    for (const SessionEntry& session : sessions) {
        if (session.Archived != view.ViewArchive) {
            continue;
        }
        const QString key = SessionKey(session.Agent, session.Id);
        const bool opened = key == view.Opened;
        const bool active = session.State != DONE;

        ++result.Counts.All;
        result.Counts.Active += active ? 1 : 0;
        result.Counts.Unread += session.Unread ? 1 : 0;

        if (view.Tab == ETab::Active && !active && !opened) {
            continue;
        }
        if (view.Tab == ETab::Unread && !session.Unread && !opened) {
            continue;
        }
        if (!Matches(session, words)) {
            continue;
        }
        Group& group = groups[session.Category];
        group.Members.push_back(&session);
        group.Unread += session.Unread ? 1 : 0;
        group.Ranked += session.Ranked ? 1 : 0;
    }

    // As a dict built from enumerate: a repeated category takes its last place.
    std::map<QString, int> pins;
    for (int i = 0; i < view.Pinned.size(); ++i) {
        pins.insert_or_assign(view.Pinned[i], i);
    }
    const int unpinned = static_cast<int>(pins.size());

    std::vector<const std::pair<const QString, Group>*> order;
    for (const auto& group : groups) {
        order.push_back(&group);
    }
    auto sortKey = [&](const auto* group) {
        const auto pin = pins.find(group->first);
        const bool pinned = pin != pins.end();
        return std::make_tuple(pinned ? pin->second : unpinned,
                               pinned ? 0 : -group->second.Ranked,
                               group->first.isEmpty(), group->first);
    };
    std::ranges::stable_sort(order, [&](const auto* a, const auto* b) {
        return sortKey(a) < sortKey(b);
    });

    for (const auto* entry : order) {
        const QString& category = entry->first;
        const Group& group = entry->second;
        const bool folded = view.Folded.contains(category);

        Row header;
        header.Kind = ERowKind::Header;
        header.Key = QStringLiteral("header:") + category;
        header.Category = category;
        header.Count = static_cast<int>(group.Members.size());
        header.UnreadCount = group.Unread;
        header.Folded = folded;
        header.Pinned = pins.contains(category);
        result.Rows.push_back(std::move(header));

        if (folded) {
            continue;
        }
        // Stable: the ranked first, the rest in the order given.
        std::vector<const SessionEntry*> members = group.Members;
        std::ranges::stable_partition(
            members, [](const SessionEntry* s) { return s->Ranked; });
        for (const SessionEntry* session : members) {
            result.Rows.push_back(
                SessionRow(*session, SessionKey(session->Agent, session->Id),
                           view.Opened));
        }
    }
    return result;
}

}  // namespace roost::sessions
