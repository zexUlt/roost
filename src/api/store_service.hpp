#pragma once

#include <QFuture>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <cstdint>

#include "api/bus_service.hpp"
#include "core/rpc.hpp"
#include "terminal/io_thread.hpp"

namespace roost::api {

// The documents of the plugins (store.get and store.set, design.md §4, §8),
// cached for the plugins that have been read. Not the daemon's
// roost::StoreService, which keeps the files: this one is the window's copy.
// Use it from the GUI thread; the signal is emitted there.
//
// A document is warm once Get has read it. From then on it is kept fresh by
// store.changed {plugin, document}, which carries the whole document, so Get
// answers from the cache without asking roostd. Set never touches the cache:
// the event that roostd publishes after saving does, for this window and for
// every other.
//
// Why "the last received wins" is right: roostd answers a request and
// publishes an event in the order it handled them, and the one socket, the
// IO thread and the GUI thread's event queue all keep that order (the answer
// of a request and the signal of an event are both posted to the GUI thread
// from the IO thread's handler of the frame), so what the GUI thread receives
// last is what roostd did last.
//
// A cache of roostd: Api calls Start once the connection is up, and the
// documents and the bus subscription are dropped when it is lost, and Start
// begins again after the reconnect: a document is read anew, not kept stale.
class StoreService : public QObject {
    Q_OBJECT

public:
    StoreService(IoThread& io, BusService& bus, QObject* parent = nullptr);

    // The connection is up: from now on what Get reads is cached. Api calls
    // it after each hello; a second call with no loss in between does nothing.
    void Start();

    // {document} as roostd answers, from the cache if the plugin is warm;
    // otherwise asked of roostd, and the document is kept. roostd's errors
    // (InvalidParams for a plugin that is not loaded, StoreFailed) pass
    // unchanged and keep nothing. Before Start nothing is kept, since nothing
    // would keep it fresh. The future finishes in the IO thread when roostd
    // is asked, so attach a continuation with a context.
    QFuture<rpc::Result> Get(const QString& plugin);

    // store.set: roostd's answer or error, unchanged. The document is a JSON
    // object, which roostd checks. Only the event updates the cache.
    QFuture<rpc::Result> Set(const QString& plugin, const QJsonValue& document);

    bool IsWarm(const QString& plugin) const;

signals:
    // The event of roostd replaced the cached document of the plugin.
    void Changed(const QString& plugin);

private:
    void Subscribe();
    void OnChanged(const QJsonValue& payload);
    void OnDisconnected();

    IoThread& Io_;
    BusService& Bus_;
    // The document of each warm plugin.
    QHash<QString, QJsonObject> Cache_;
    bool Started_ = false;
    // Counts the connections lost; an answer of an older one is not ours.
    std::uint64_t Epoch_ = 0;
    // Zero while not subscribed to store.changed.
    BusService::LocalId Subscription_ = 0;
};

}  // namespace roost::api
