#pragma once

#include <QFuture>
#include <QJsonValue>
#include <QMetaObject>
#include <QObject>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/rpc.hpp"
#include "terminal/io_thread.hpp"

namespace roost::api {

// The event bus of roostd seen from the window (design.md §2, §4). Listeners
// are local; the service keeps one bus.subscribe at roostd for each distinct
// pattern while it has a listener, and routes the bus.event notifications to
// the listeners of the pattern. Use it from the GUI thread; callbacks run
// there, in the order the listeners subscribed to the same pattern; across
// patterns there is no promised order. A callback may subscribe and
// unsubscribe.
//
// No reconnect yet: after the connection is lost the listeners stay but hear
// nothing (Unsubscribe still works), and a new subscription makes a fresh
// bus.subscribe, which fails: a warning is logged and its listeners are
// dropped.
class BusService : public QObject {
    Q_OBJECT

public:
    using LocalId = std::uint64_t;
    using Listener =
        std::function<void(const QString& name, const QJsonValue& payload)>;

    // Events queued for a subscription whose bus.subscribe answer has not
    // come yet; the most that is kept.
    static constexpr std::size_t MAX_PARKED = 256;

    explicit BusService(IoThread& io, QObject* parent = nullptr);

    // A pattern as EventBus takes it: a name, "prefix.*" or "*". InvalidParams
    // for any other, or for a context that lives in another thread. When
    // `context` is destroyed the subscription goes with it; null means it
    // lasts until Unsubscribe. Nothing is called before this returns.
    std::expected<LocalId, rpc::Error> Subscribe(const QString& pattern,
                                                 const QObject* context,
                                                 Listener listener);

    // Nothing for an id that is not subscribed. The listener is not called
    // again, even for events already on their way. The last listener of a
    // pattern takes the bus.subscribe of roostd with it.
    void Unsubscribe(LocalId id);

    // bus.publish; roostd's answer or error. The future finishes in the IO
    // thread, so attach a continuation with a context.
    QFuture<rpc::Result> Publish(const QString& name,
                                 const QJsonValue& payload);

private:
    struct Local {
        QString Pattern;
        // Shared so that a listener that unsubscribes itself is not destroyed
        // while it runs.
        std::shared_ptr<const Listener> Call;
        QMetaObject::Connection Guard;  // the destruction of the context
    };
    // One bus.subscribe of roostd.
    struct Remote {
        // Empty while the answer is pending.
        std::optional<qint64> Subscription;
        std::vector<LocalId> Locals;
    };
    struct Parked {
        qint64 Subscription = 0;
        QString Name;
        QJsonValue Payload;
    };

    void OnNotified(const QString& method, const QJsonValue& params);
    void OnSubscribed(const QString& pattern, const rpc::Result& answer);
    // Forgets all that is roostd's: the connection is gone and its
    // subscriptions with it. The local listeners stay, silent.
    void OnDisconnected();
    void Deliver(const QString& pattern, const QString& name,
                 const QJsonValue& payload);
    void Forget(LocalId id);
    void Drop(LocalId id);
    void SendUnsubscribe(qint64 subscription);

    IoThread& Io_;
    LocalId NextId_ = 1;
    // Ordered, so that the listeners of a pattern hear in the order they
    // subscribed (across patterns the order is not promised).
    std::map<LocalId, Local> Locals_;
    std::map<QString, Remote> Remotes_;
    // roostd's subscription -> its pattern; only answered ones.
    std::unordered_map<qint64, QString> Routes_;
    // Subscriptions whose answer is pending; while any, events for an unknown
    // subscription may belong to one of them and are parked.
    int Pending_ = 0;
    // Counts the connections lost; an answer of an older one is not ours.
    std::uint64_t Epoch_ = 0;
    std::vector<Parked> Parked_;
};

}  // namespace roost::api
