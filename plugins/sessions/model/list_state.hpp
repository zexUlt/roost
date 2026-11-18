#pragma once

#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

#include "sessions/model/session_rows.hpp"

// What the sessions list remembers (the store document `sessions`, v1) and
// what it derives from the changes of the session states: categories, names,
// archive, colours, pins, folds, unread and the view. In memory only: the
// store, the Qt model and the QML sit on top.
namespace roost::sessions {

// Colours handed out to categories: indices 0 .. COLOR_COUNT - 1.
inline constexpr int COLOR_COUNT = 8;
inline constexpr int DOCUMENT_VERSION = 1;

class ListState {
public:
    // Replaces everything with the document. A missing or unreadable section
    // is empty, a malformed entry is dropped alone, a repeated pin keeps its
    // last position. A `version` that is missing is 1; an integer 1 is read
    // and written; anything else (above ours, below 1, fractional, not a
    // number) keeps the state readable but sets ReadOnly(): the caller must
    // not write such a state back. A document without `states` (or with a
    // `states` object of nothing but malformed entries) is a first run: the
    // next Update seeds the states and marks nothing unread.
    //
    // The view (opened, query, tab, archive, compact, pins, folds) is always
    // the document's: a store change overwrites what the window shows. The
    // ranking is kept, renewed only by the rule of Update.
    //
    // Returns whether the state, reconciled with the listed sessions, differs
    // from the given document, so the caller should save it. Always false
    // when read-only.
    bool Restore(const QJsonObject& document);
    // The document to save; nullopt when read-only.
    std::optional<QJsonObject> ToDocument() const;

    bool ReadOnly() const { return !Problem_.isEmpty(); }
    // Why the state is read-only; empty otherwise.
    const QString& Problem() const { return Problem_; }

    // The sessions as `sessions.list` gives them (Agent, Id, AgentTitle,
    // State, Cwd, StartedAt; the rest is ignored). Marks the sessions whose
    // state moved to something but `working` as unread, never the opened one,
    // and renews the ranking if a new unread appeared. Entries of sessions
    // that are gone stay: removal can be transient. Returns whether the
    // document changed.
    bool Update(std::vector<SessionEntry> listed);

    // The listed sessions with the name, category, archive flag and unread
    // flags filled in, in the order of Update; the input of BuildRows.
    std::vector<SessionEntry> Entries() const;
    const ViewState& View() const { return View_; }
    bool Compact() const { return Compact_; }

    // The colour index of a category; -1 for "" and for a category with none.
    int ColorIndex(const QString& category) const;

    // The edits return whether the document changed and should be saved. They
    // work in a read-only state too (the list stays usable) but return false
    // there. A key that is not `agent/id` is refused: false, no change.
    //
    // "" is uncategorized. A category gets its colour at its first session.
    bool SetCategory(const QString& key, const QString& category);
    // A name equal to the agent's title, or empty, drops the given name.
    bool Rename(const QString& key, const QString& name);
    bool SetArchived(const QString& key, bool archived);
    bool TogglePin(const QString& category);
    bool ToggleFold(const QString& category);
    bool UnfoldAll();
    bool SetQuery(const QString& query);
    bool SetTab(ETab tab);
    bool SetViewArchive(bool archive);
    bool SetCompact(bool compact);
    // Opening reads the session. Empty key: nothing is opened.
    bool SetOpened(const QString& key);
    // Drops everything the list knows of the session, as after its deletion,
    // including its place in the last listing: a later Update that still lists
    // it treats it as a new session, first seen (unread if not working).
    bool Forget(const QString& key);

private:
    bool IsShownUnread(const QString& key, const QString& state) const;
    QSet<QString> ShownUnread() const;
    std::optional<QJsonObject> BuildDocument() const;
    bool Saved(bool changed) const { return changed && !ReadOnly(); }
    bool Reconcile(const QSet<QString>& shownBefore);
    void RenewRanking(const QSet<QString>& shownBefore);
    void AssignColor(const QString& category);

    QHash<QString, QString> Categories_;
    QHash<QString, QString> Names_;
    QSet<QString> Archived_;
    QMap<QString, int> Colors_;
    QSet<QString> Unread_;
    QHash<QString, QString> States_;
    // The unread the order is built on, renewed when a new unread appears.
    // Not stored: the first Update after a start builds it.
    QSet<QString> Ranked_;
    ViewState View_;
    bool Compact_ = false;
    bool Seeded_ = false;
    QString Problem_;
    std::vector<SessionEntry> Listed_;
};

}  // namespace roost::sessions
