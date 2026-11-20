#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

#include <functional>
#include <optional>
#include <vector>

#include "sessions/model/list_state.hpp"
#include "sessions/model/session_rows.hpp"

namespace roost::sessions {

// The model of the sessions list as QML sees it (module RoostSessions): the
// rows of BuildRows over a source model of sessions, the cursor, and the
// store document `sessions` it keeps through ListState. It talks to Roost only
// through its properties and signals: `source` is any item model with the
// roles of Roost.sessions, the document comes in by restore() and goes out by
// saveRequested.
//
// Rows are headers and sessions, in the order of BuildRows. They change by
// removes, moves, inserts and dataChanged, never by a reset, except that a
// reset of the source is a reset here too. Nothing is written until the
// document was read (restore or readFailed) and never while the list is
// read-only.
//
// The cursor is a stop, a session or a folded header, kept by key. Rows
// appear only after the document was read and `source` is loaded.
class SessionsList : public QAbstractListModel {
    Q_OBJECT
    // Any item model with the roles agent, id, title, cwd, state, startedAt.
    Q_PROPERTY(QAbstractItemModel* source READ source WRITE setSource NOTIFY
                   sourceChanged)
    // Whether the first list of the connection is in: the rows of a reset
    // source are taken only when it is.
    Q_PROPERTY(bool sourceLoaded READ sourceLoaded WRITE setSourceLoaded NOTIFY
                   sourceLoadedChanged)
    // The open session, {agent, id} or null: never unread, kept by the tabs.
    Q_PROPERTY(QVariant opened READ opened WRITE setOpened NOTIFY openedChanged)
    // The document was read, or the read failed.
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    // A write is in flight or waits to be sent, or a sync that may ask for one.
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged)
    // Nothing is written: the read failed or the document is of a newer
    // version. `problem` says why, or that a write failed.
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY readOnlyChanged)
    Q_PROPERTY(QString problem READ problem NOTIFY problemChanged)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    // "all", "active" or "unread".
    Q_PROPERTY(QString tab READ tab WRITE setTab NOTIFY tabChanged)
    Q_PROPERTY(bool viewArchive READ viewArchive WRITE setViewArchive NOTIFY
                   viewArchiveChanged)
    Q_PROPERTY(bool compact READ compact WRITE setCompact NOTIFY compactChanged)
    // {all, active, unread}: the sessions of the archive view, before the tab
    // and the query.
    Q_PROPERTY(QVariantMap counts READ counts NOTIFY countsChanged)
    // The categories of the listed sessions, sorted.
    Q_PROPERTY(QStringList knownCategories READ knownCategories NOTIFY
                   knownCategoriesChanged)
    // The row of the cursor, or -1.
    Q_PROPERTY(int cursorRow READ cursorRow NOTIFY cursorChanged)
    // {agent, id} when the cursor is on a session, else null.
    Q_PROPERTY(QVariant cursorSession READ cursorSession NOTIFY cursorChanged)
    // The category of a folded header or of the cursor's session, else null.
    Q_PROPERTY(QVariant cursorCategory READ cursorCategory NOTIFY cursorChanged)

public:
    explicit SessionsList(QObject* parent = nullptr);

    // The roles of the source this model reads: the fields of sessions.list.
    static QStringList sourceRoleNames();

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QAbstractItemModel* source() const;
    void setSource(QAbstractItemModel* source);
    bool sourceLoaded() const;
    void setSourceLoaded(bool loaded);
    QVariant opened() const;
    void setOpened(const QVariant& ref);
    bool ready() const;
    bool saving() const;
    bool readOnly() const;
    QString problem() const;
    QString query() const;
    void setQuery(const QString& query);
    QString tab() const;
    void setTab(const QString& tab);
    bool viewArchive() const;
    void setViewArchive(bool archive);
    bool compact() const;
    void setCompact(bool compact);
    QVariantMap counts() const;
    QStringList knownCategories() const;
    int cursorRow() const;
    QVariant cursorSession() const;
    QVariant cursorCategory() const;

    // The cursor. move: to the stop `step` stops away, not past the first or
    // the last. setCursorRow: to the stop on the row (a tap), false if the
    // row is no stop.
    Q_INVOKABLE void move(int step);
    Q_INVOKABLE bool setCursorRow(int row);
    // The row of a session {agent, id}, or -1; and the session of a row, null
    // on a header.
    Q_INVOKABLE int rowOf(const QVariant& ref) const;
    Q_INVOKABLE QVariant refAt(int row) const;

    // Folds, pins, tabs. A category is its name, "" the uncategorized.
    Q_INVOKABLE void toggleFold(const QString& category);
    Q_INVOKABLE void unfoldAll();
    Q_INVOKABLE void togglePin(const QString& category);
    Q_INVOKABLE void nextTab();

    // Edits of a session {agent, id}; a session that is not listed (yet) is
    // fine. forget drops all the list knows of it after its deletion; reveal
    // clears the query, shows the archive view the session is in, unfolds its
    // category and puts the cursor on it (false if it is not listed).
    Q_INVOKABLE void setCategory(const QVariant& ref, const QString& name);
    Q_INVOKABLE void rename(const QVariant& ref, const QString& name);
    Q_INVOKABLE void setArchived(const QVariant& ref, bool archived);
    Q_INVOKABLE void forget(const QVariant& ref);
    Q_INVOKABLE bool reveal(const QVariant& ref);

    // The store. restore: the document, from a read or from a change of the
    // store (empty if there is none); the echo of our own write is ignored,
    // any other document replaces the state. readFailed: the read failed, the
    // list works and is read-only. saveFinished: the write that saveRequested
    // asked for ended, with an error text or an empty one. storeLost: the
    // connection to the store went; nothing is written until restore or
    // readFailed, so that a write of the state kept from before does not
    // overtake the read of the document at the next connection.
    Q_INVOKABLE void restore(const QVariantMap& document);
    Q_INVOKABLE void readFailed(const QString& error);
    Q_INVOKABLE void saveFinished(const QString& error);
    Q_INVOKABLE void storeLost();

    // The icon of an agent for the row of its session (agent_icon.hpp): a file
    // URL of the user's folder or a qrc URL of the bundled icon. Kept per
    // agent for the life of this list: the folder is read again when the panel
    // is created, not when a file appears.
    Q_INVOKABLE QUrl agentIcon(const QString& agent);

    // The bundled icon of an agent (or the bundled default): what the row
    // shows when the file of the user fails to load.
    Q_INVOKABLE QUrl bundledAgentIcon(const QString& agent) const;

signals:
    void sourceChanged();
    void sourceLoadedChanged();
    void openedChanged();
    void readyChanged();
    void savingChanged();
    void readOnlyChanged();
    void problemChanged();
    void queryChanged();
    void tabChanged();
    void viewArchiveChanged();
    void compactChanged();
    void countsChanged();
    void knownCategoriesChanged();
    void cursorChanged();
    // The document to write; one in flight, the next after saveFinished.
    void saveRequested(const QVariantMap& document);

private:
    struct Exposed;

    void scheduleSync();
    void sync();
    void changing(const std::function<void()>& body);
    // The edit of a state: applies it, rebuilds the rows, asks for a save if
    // the document changed. Ignored until the document was read.
    void edit(const std::function<bool()>& apply);
    void publish(bool asReset, bool hold);
    void applySteps(const std::vector<Row>& rows);
    void reconcileCursor(const std::vector<QString>& oldStops, bool reset);
    void setCursor(const QString& key);
    void requestSave();
    void retrySave();
    void sendSave();
    std::vector<SessionEntry> readSource() const;
    // The category of a listed session, nullopt if it is not listed.
    std::optional<QString> listedCategory(const QString& key) const;
    bool categoryListed(const QString& category) const;
    bool holding() const;
    void syncBody();
    std::optional<QString> cursorCategoryOf() const;
    int indexOfRow(const QString& key) const;
    std::vector<QString> stops() const;
    Exposed snapshot() const;
    void announce(const Exposed& before);

    ListState State_;
    std::vector<Row> Rows_;
    TabCounts Counts_;
    QStringList Known_;
    QHash<QString, int> ShownColors_;
    QHash<QString, QUrl> AgentIcons_;

    QPointer<QAbstractItemModel> Source_;
    bool SourceLoaded_ = false;
    // The source was reset and its rows were not taken yet.
    bool ResetPending_ = false;
    bool SyncQueued_ = false;

    bool Ready_ = false;
    bool ReadFailed_ = false;
    QString ReadProblem_;
    QString SaveProblem_;
    // opened, set before the document was read.
    std::optional<QVariant> EarlyOpened_;

    QString CursorKey_;

    bool InFlight_ = false;
    bool Dirty_ = false;
    // The store was lost and the document is not read again yet: the state
    // may be older than the store's, so it is not written.
    bool Reading_ = false;
    // The document in flight, until its change event comes back or the answer
    // does: roostd announces every successful write, equal or not, and does
    // so before it answers, so no echo outlives its write.
    std::optional<QJsonObject> Echo_;
};

}  // namespace roost::sessions
