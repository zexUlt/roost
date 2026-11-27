#include "roostd/bus_subscriptions.hpp"

#include <QJsonObject>

#include <memory>
#include <utility>

namespace roost {

namespace {

rpc::Result InvalidParams(const QString& message) {
    return std::unexpected(
        rpc::Error::FromCode(rpc::EErrorCode::InvalidParams, message));
}

}  // namespace

BusSubscriptions::BusSubscriptions(EventBus& bus) : Bus_(bus) {}

BusSubscriptions::~BusSubscriptions() { Clear(); }

std::optional<EventBus::SubscriptionId> BusSubscriptions::Subscribe(
    const QString& pattern, Sink sink) {
    // The listener needs its own id, which exists only after Subscribe.
    auto self = std::make_shared<EventBus::SubscriptionId>(0);
    const auto id = Bus_.Subscribe(
        pattern, [self, sink = std::move(sink)](const QString& name,
                                                const QJsonValue& payload) {
            sink(*self, name, payload);
        });
    if (id) {
        *self = *id;
        Ids_.insert(*id);
    }
    return id;
}

bool BusSubscriptions::Unsubscribe(EventBus::SubscriptionId id) {
    if (Ids_.erase(id) == 0) {
        return false;
    }
    return Bus_.Unsubscribe(id);
}

void BusSubscriptions::Clear() {
    for (const EventBus::SubscriptionId id : std::exchange(Ids_, {})) {
        Bus_.Unsubscribe(id);
    }
}

rpc::Result BusSubscriptions::HandleSubscribe(const QJsonValue& params,
                                              Sink sink) {
    const QJsonValue pattern =
        params.toObject().value(QStringLiteral("pattern"));
    const auto id = pattern.isString()
                        ? Subscribe(pattern.toString(), std::move(sink))
                        : std::nullopt;
    if (!id) {
        return InvalidParams(
            QStringLiteral("bus.subscribe takes {pattern}: an event name, a "
                           "prefix ending in \".*\" or \"*\""));
    }
    return QJsonObject{
        {QStringLiteral("subscription"), static_cast<qint64>(*id)},
    };
}

rpc::Result BusSubscriptions::HandleUnsubscribe(const QJsonValue& params) {
    const std::optional<qint64> id =
        rpc::ToInteger(params.toObject().value(QStringLiteral("subscription")));
    if (!id || *id < 0 ||
        !Unsubscribe(static_cast<EventBus::SubscriptionId>(*id))) {
        return InvalidParams(
            QStringLiteral("bus.unsubscribe takes {subscription}: an id this "
                           "connection got from bus.subscribe"));
    }
    return QJsonValue();
}

}  // namespace roost
