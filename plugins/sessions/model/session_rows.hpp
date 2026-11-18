#pragma once

#include <QSet>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <vector>

// The rows of the sessions list (design.md §6) as a pure function: sessions
// and the state of the view in, headers and sessions out. No store, no Qt
// model; the list model and the store document build on it.
namespace roost::sessions {

// One session as the list sees it. Unread, Ranked, Category, Name and Archived
// come from the list's own state; the rest from `sessions.list`.
struct SessionEntry {
    QString Agent;
    QString Id;
    // The adapter's title.
    QString AgentTitle;
    // The name the user gave; empty if none.
    QString Name;
    QString State;
    QString Cwd;
    qint64 StartedAt = 0;
    // "" is uncategorized.
    QString Category;
    // Shown as unread: already not working and not opened. Drives the Unread
    // tab, the counts and the display, never the order.
    bool Unread = false;
    // Ranks the session as unread for the order only: it moves the group up
    // and the session to the top of its group. The caller keeps it frozen
    // (renewed only when a new unread appears), so that a session just read
    // does not jump under the cursor. Not computed here.
    bool Ranked = false;
    bool Archived = false;
};

enum class ETab { All, Active, Unread };

struct ViewState {
    ETab Tab = ETab::All;
    // Words, all of which must be found; empty shows everything.
    QString Query;
    // Archived sessions are shown instead of the others.
    bool ViewArchive = false;
    // Key of the opened session ("agent/id"); empty if none. It stays in
    // the Active and Unread tabs.
    QString Opened;
    // Categories in the order they were pinned; "" is uncategorized.
    QStringList Pinned;
    QSet<QString> Folded;
};

enum class ERowKind { Header, Session };

struct Row {
    ERowKind Kind = ERowKind::Session;
    // "agent/id" for a session, "header:<category>" for a header.
    QString Key;
    QString Category;

    // Session rows.
    QString Agent;
    QString Id;
    // The given name, else the adapter's title.
    QString Title;
    QString AgentTitle;
    QString State;
    QString Cwd;
    qint64 StartedAt = 0;
    bool Unread = false;
    bool IsOpen = false;
    bool Archived = false;

    // Header rows: the sessions of the group, folded ones included.
    int Count = 0;
    int UnreadCount = 0;
    bool Folded = false;
    bool Pinned = false;

    bool operator==(const Row&) const = default;
};

// Sessions of the current archive view, before the tab and the query.
struct TabCounts {
    int All = 0;
    int Active = 0;
    int Unread = 0;

    bool operator==(const TabCounts&) const = default;
};

struct RowsResult {
    std::vector<Row> Rows;
    TabCounts Counts;
};

// "agent/id": agent ids hold no '/', so the first one splits the key.
QString SessionKey(const QString& agent, const QString& id);

// Filters by the archive view, the tab and the query (every lower-cased
// word is a substring of "title state category", the category being
// "uncategorized" for ""), groups by category and flattens.
//
// Groups: pinned first in pin order (a category pinned twice takes its last
// place); then by the count of Ranked sessions, descending; the uncategorized
// last among equals; then by name. A group with no session left has no
// header. Inside a group the Ranked come first and the rest keep the order of
// `sessions`. The Unread tab and the counts use Unread, not Ranked. A folded
// category is its header only.
RowsResult BuildRows(const std::vector<SessionEntry>& sessions,
                     const ViewState& view);

}  // namespace roost::sessions
