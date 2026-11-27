#pragma once

#include <QJsonValue>
#include <QString>

#include <cstddef>
#include <functional>
#include <optional>
#include <set>

#include "core/bus.hpp"
#include "core/rpc.hpp"

namespace roost {

// The bus subscriptions of one holder, a socket connection or a plugin
// backend (design.md §4, §5), and the bus.subscribe and bus.unsubscribe
// methods over them, so that both sides answer alike. They all end with the
// holder: the destructor and Clear() remove them from the bus.
class BusSubscriptions {
public:
    // Called with the subscription the event came through.
    using Sink =
        std::function<void(EventBus::SubscriptionId id, const QString& name,
                           const QJsonValue& payload)>;

    explicit BusSubscriptions(EventBus& bus);
    ~BusSubscriptions();

    BusSubscriptions(const BusSubscriptions&) = delete;
    BusSubscriptions& operator=(const BusSubscriptions&) = delete;

    // Nothing if the pattern is not valid.
    std::optional<EventBus::SubscriptionId> Subscribe(const QString& pattern,
                                                      Sink sink);
    // False unless the id is one of this holder's.
    bool Unsubscribe(EventBus::SubscriptionId id);
    // Removes all of them from the bus.
    void Clear();

    std::size_t Count() const { return Ids_.size(); }

    // bus.subscribe {pattern} -> {subscription}; InvalidParams if the params
    // do not carry a valid pattern.
    rpc::Result HandleSubscribe(const QJsonValue& params, Sink sink);
    // bus.unsubscribe {subscription} -> null; InvalidParams if the params do
    // not carry an id of this holder's.
    rpc::Result HandleUnsubscribe(const QJsonValue& params);

private:
    EventBus& Bus_;
    std::set<EventBus::SubscriptionId> Ids_;
};

}  // namespace roost
