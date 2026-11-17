#pragma once

#include <QFuture>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <utility>

#include "api/bus_service.hpp"
#include "core/rpc.hpp"
#include "terminal/io_thread.hpp"

namespace roost::api {

// The sessions of all agents (sessions.list, design.md §4), kept as rows: one
// QJsonObject per session with the fields roostd gives it, {agent, id, title,
// cwd, state, startedAt?}. The model roles of Roost.sessions are these keys
// (FIELDS). A row is identified by (agent, id); the rows keep the order of
// roostd's answer, and a session that is new is appended.
//
// Kept fresh by the session.stateChanged events, which carry a whole row, and
// the session.removed events {agent, id}, which drop one. Refresh replaces all
// the rows; there is no polling. Use it from the GUI thread; the signals are
// emitted there.
//
// An event that comes while a list is being asked for is put on the rows again
// when the answer has replaced them, in the order it came: roostd answers when
// every agent has, and the list may be older than the event. So a session that
// changed and then went away is gone from the list that answers, too.
//
// A cache of roostd: Api calls Start once the connection is up, and the rows
// and the bus subscription are dropped when it is lost; Start makes them
// again after the reconnect, so there is one subscription at a time.
class SessionService : public QObject {
    Q_OBJECT

public:
    // The keys of a row, startedAt being the one that may be absent.
    static inline const QStringList FIELDS = {
        QStringLiteral("agent"), QStringLiteral("id"),
        QStringLiteral("title"), QStringLiteral("cwd"),
        QStringLiteral("state"), QStringLiteral("startedAt")};

    SessionService(IoThread& io, BusService& bus, QObject* parent = nullptr);

    // The connection is up: subscribes to the events and fetches the list.
    // Api calls it after each hello; a second call with no loss in between
    // does nothing.
    void Start();

    bool IsLoaded() const;

    // The rows, in roostd's order; empty until the first answer.
    const QList<QJsonObject>& Rows() const;
    // The agents whose adapter did not answer with a list, as roostd sent
    // them: [{agent, reason}].
    const QJsonArray& Unavailable() const;
    // The row of (agent, id), or -1.
    int IndexOf(const QString& agent, const QString& id) const;

    // Asks roostd for the list; always, never from the cache. The future is
    // roostd's answer {sessions, unavailable}, or its error. A good answer
    // replaces the rows and Reset follows; a failed one leaves them as they
    // are. The future finishes in the IO thread, so attach a continuation
    // with a context.
    QFuture<rpc::Result> Refresh();

signals:
    // The rows were replaced: re-read all of them.
    void Reset();
    void RowChanged(int index);
    void RowInserted(int index);
    // The row is still there when the first is emitted, and gone when the
    // second is; the rows after it have moved up by one.
    void RowAboutToBeRemoved(int index);
    void RowRemoved(int index);

private:
    using Key = std::pair<QString, QString>;

    // An event kept for the list that answers: the row of a change, or the
    // {agent, id} of a removal.
    struct KeptEvent {
        bool Removed = false;
        QJsonObject Row;
    };

    void OnEvent(const QString& name, const QJsonValue& payload);
    // The answer of the request that was the order-th asked: replaces the rows
    // if it is good and not stale, then puts back the events it may be older
    // than.
    void OnSettled(std::uint64_t order, const rpc::Result& answer);
    void OnAnswer(const QJsonValue& answer);
    // Puts a row of an event in its place and tells of it.
    void ApplyRow(const QJsonObject& row);
    // Takes a row away and tells of it; nothing for a session not in the rows.
    void RemoveRow(const Key& key);
    // The connection is lost: the rows were a copy of what is gone.
    void OnDisconnected();

    IoThread& Io_;
    BusService& Bus_;
    QList<QJsonObject> Rows_;
    QHash<Key, int> Index_;
    QJsonArray Unavailable_;
    bool Loaded_ = false;
    bool Started_ = false;
    // The requests of the list that are out, the number asked and the order of
    // the newest answer applied. The events that came while any is out are
    // kept in Kept_, in the order they came, to be put on the list that
    // answers: it may be older than they are.
    int InFlight_ = 0;
    std::uint64_t Issued_ = 0;
    std::uint64_t Applied_ = 0;
    QList<KeptEvent> Kept_;
    // Counts the connections lost; an answer of an older one is not ours.
    std::uint64_t Epoch_ = 0;
    BusService::LocalId Subscription_ = 0;
};

}  // namespace roost::api
