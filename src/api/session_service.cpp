#include "api/session_service.hpp"

#include <QtLogging>

#include <utility>

#include "api/thread_check.hpp"

namespace roost::api {

namespace {

const QString AGENT = QStringLiteral("agent");
const QString ID = QStringLiteral("id");

}  // namespace

SessionService::SessionService(IoThread& io, BusService& bus, QObject* parent)
    : QObject(parent), Io_(io), Bus_(bus) {
    connect(&Io_, &IoThread::Disconnected, this,
            &SessionService::OnDisconnected);
}

void SessionService::Start() {
    if (!CheckThread(*this, "SessionService::Start") ||
        std::exchange(Started_, true)) {
        return;
    }
    // Before the first list is asked for, so that no change is missed between
    // the answer and the subscription. One subscription for both events: the
    // bus keeps no order across patterns, and a change then a removal of one
    // session must arrive in that order.
    const auto subscription =
        Bus_.Subscribe(QStringLiteral("session.*"), this,
                       [this](const QString& name, const QJsonValue& payload) {
                           OnEvent(name, payload);
                       });
    if (subscription) {
        Subscription_ = *subscription;
    }
    Refresh();
}

bool SessionService::IsLoaded() const {
    return CheckThread(*this, "SessionService::IsLoaded") && Loaded_;
}

const QList<QJsonObject>& SessionService::Rows() const {
    static const QList<QJsonObject> NONE;
    return CheckThread(*this, "SessionService::Rows") ? Rows_ : NONE;
}

const QJsonArray& SessionService::Unavailable() const {
    static const QJsonArray NONE;
    return CheckThread(*this, "SessionService::Unavailable") ? Unavailable_
                                                             : NONE;
}

int SessionService::IndexOf(const QString& agent, const QString& id) const {
    if (!CheckThread(*this, "SessionService::IndexOf")) {
        return -1;
    }
    return Index_.value(Key{agent, id}, -1);
}

QFuture<rpc::Result> SessionService::Refresh() {
    if (!CheckThread(*this, "SessionService::Refresh")) {
        return rpc::Ready(std::unexpected(WrongThread()));
    }
    // From now until the answer, the events are kept: roostd answers when
    // every adapter has, and an adapter that was quick may have said more than
    // its part of the list does.
    ++InFlight_;
    return Io_.Request(QStringLiteral("sessions.list"))
        .then(this,
              [this, epoch = Epoch_, order = ++Issued_](rpc::Result answer) {
                  if (epoch == Epoch_) {
                      OnSettled(order, answer);
                  }
                  return answer;
              });
}

void SessionService::OnSettled(std::uint64_t order, const rpc::Result& answer) {
    // An answer older than the one applied is stale, though it came later.
    const bool apply = answer && order > Applied_;
    QList<KeptEvent> replay;
    if (apply) {
        replay = Kept_;
    }
    // The kept events reach back to the earliest request still out.
    if (--InFlight_ == 0) {
        Kept_.clear();
    }
    if (!apply) {
        return;
    }
    Applied_ = order;
    OnAnswer(*answer);
    // Each event carries a whole row, so one that the answer already knows of
    // changes nothing, and one that is newer than the answer's row wins. The
    // rows are told one by one, as they are for an event that is live; a
    // removal that came after a row takes it away again. This assumes that a
    // session which came back is announced by its own stateChanged: if roostd
    // dropped it, the replayed removal stays in force until the next refresh.
    for (const KeptEvent& event : replay) {
        if (event.Removed) {
            RemoveRow(Key{event.Row.value(AGENT).toString(),
                          event.Row.value(ID).toString()});
        } else {
            ApplyRow(event.Row);
        }
    }
}

void SessionService::OnAnswer(const QJsonValue& answer) {
    const QJsonObject object = answer.toObject();
    Rows_.clear();
    Index_.clear();
    for (const QJsonValue& value : object.value("sessions").toArray()) {
        const QJsonObject row = value.toObject();
        const Key key{row.value(AGENT).toString(), row.value(ID).toString()};
        // roostd lists each session once; were it to repeat one, the later
        // row would stand in for the earlier at its place.
        const auto known = Index_.constFind(key);
        if (known != Index_.cend()) {
            Rows_[*known] = row;
            continue;
        }
        Index_.insert(key, static_cast<int>(Rows_.size()));
        Rows_.append(row);
    }
    Unavailable_ = object.value("unavailable").toArray();
    Loaded_ = true;
    emit Reset();
}

void SessionService::OnEvent(const QString& name, const QJsonValue& payload) {
    if (name == QStringLiteral("session.adapterStarted")) {
        // What an adapter did while it was down is told by no event. The events
        // kept so far are older than the restart: the list that is asked for
        // now is the one to trust.
        Kept_.clear();
        Refresh();
        return;
    }
    const bool removed = name == QStringLiteral("session.removed");
    if (!removed && name != QStringLiteral("session.stateChanged")) {
        return;
    }
    const QJsonObject row = payload.toObject();
    if (row.value(AGENT).toString().isEmpty() ||
        row.value(ID).toString().isEmpty()) {
        qWarning("%s without an agent or an id", qUtf8Printable(name));
        return;
    }
    if (InFlight_ > 0) {
        Kept_.append({removed, row});
    }
    // Until the first list has come, the list that comes replaces whatever an
    // event would have said; but it is kept above, as the list may be older.
    if (!Loaded_) {
        return;
    }
    if (removed) {
        RemoveRow(Key{row.value(AGENT).toString(), row.value(ID).toString()});
    } else {
        ApplyRow(row);
    }
}

void SessionService::RemoveRow(const Key& key) {
    const auto known = Index_.constFind(key);
    if (known == Index_.cend()) {
        return;
    }
    const int index = *known;
    // The model must hear of it before the row is gone.
    emit RowAboutToBeRemoved(index);
    Rows_.removeAt(index);
    Index_.remove(key);
    for (int i = index; i < Rows_.size(); ++i) {
        Index_.insert(Key{Rows_[i].value(AGENT).toString(),
                          Rows_[i].value(ID).toString()},
                      i);
    }
    emit RowRemoved(index);
}

void SessionService::ApplyRow(const QJsonObject& row) {
    const Key key{row.value(AGENT).toString(), row.value(ID).toString()};
    const auto known = Index_.constFind(key);
    if (known != Index_.cend()) {
        Rows_[*known] = row;
        emit RowChanged(*known);
        return;
    }
    const auto index = static_cast<int>(Rows_.size());
    Index_.insert(key, index);
    Rows_.append(row);
    emit RowInserted(index);
}

void SessionService::OnDisconnected() {
    ++Epoch_;
    InFlight_ = 0;
    Kept_.clear();
    Started_ = false;
    Bus_.Unsubscribe(std::exchange(Subscription_, 0));
    const bool hadRows = std::exchange(Loaded_, false);
    Rows_.clear();
    Index_.clear();
    Unavailable_ = {};
    if (hadRows) {
        emit Reset();
    }
}

}  // namespace roost::api
