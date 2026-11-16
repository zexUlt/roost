#include "api/bus_service.hpp"

#include <QJsonObject>
#include <QtLogging>

#include <algorithm>
#include <utility>

#include "api/thread_check.hpp"
#include "core/bus.hpp"

namespace roost::api {

namespace {

rpc::Error InvalidParams(QString message) {
    return rpc::Error::FromCode(rpc::EErrorCode::InvalidParams,
                                std::move(message));
}

}  // namespace

BusService::BusService(IoThread& io, QObject* parent)
    : QObject(parent), Io_(io) {
    connect(&Io_, &IoThread::Notified, this, &BusService::OnNotified);
    connect(&Io_, &IoThread::Disconnected, this, &BusService::OnDisconnected);
}

std::expected<BusService::LocalId, rpc::Error> BusService::Subscribe(
    const QString& pattern, const QObject* context, Listener listener) {
    if (!CheckThread(*this, "BusService::Subscribe")) {
        return std::unexpected(WrongThread());
    }
    if (!EventBus::IsValidPattern(pattern)) {
        return std::unexpected(InvalidParams(
            QStringLiteral("a pattern is an event name, a prefix ending in "
                           "\".*\" or \"*\"")));
    }
    if (context != nullptr && context->thread() != thread()) {
        return std::unexpected(InvalidParams(
            QStringLiteral("the context lives in another thread")));
    }
    const LocalId id = NextId_++;
    Local local{
        .Pattern = pattern,
        .Call = std::make_shared<const Listener>(std::move(listener)),
        .Guard = {},
    };
    if (context != nullptr) {
        local.Guard = connect(context, &QObject::destroyed, this,
                              [this, id] { Drop(id); });
    }
    Locals_.emplace(id, std::move(local));
    const auto [remote, created] = Remotes_.try_emplace(pattern);
    remote->second.Locals.push_back(id);
    if (created) {
        // A pattern that is already pending takes this listener along: its
        // answer finds it in the list.
        ++Pending_;
        Io_.Request(QStringLiteral("bus.subscribe"),
                    QJsonObject{{QStringLiteral("pattern"), pattern}})
            .then(this,
                  [this, pattern, epoch = Epoch_](const rpc::Result& answer) {
                      if (epoch == Epoch_) {
                          OnSubscribed(pattern, answer);
                      }
                  });
    }
    return id;
}

void BusService::Unsubscribe(LocalId id) {
    if (!CheckThread(*this, "BusService::Unsubscribe")) {
        return;
    }
    Drop(id);
}

QFuture<rpc::Result> BusService::Publish(const QString& name,
                                         const QJsonValue& payload) {
    if (!CheckThread(*this, "BusService::Publish")) {
        return rpc::Ready(std::unexpected(WrongThread()));
    }
    return Io_.Request(QStringLiteral("bus.publish"),
                       QJsonObject{{QStringLiteral("name"), name},
                                   {QStringLiteral("payload"), payload}});
}

void BusService::OnNotified(const QString& method, const QJsonValue& params) {
    if (method != QStringLiteral("bus.event")) {
        return;
    }
    const QJsonObject object = params.toObject();
    const auto subscription =
        rpc::ToInteger(object.value(QStringLiteral("subscription")));
    const QJsonValue name = object.value(QStringLiteral("name"));
    if (!subscription || !name.isString()) {
        qWarning("bus.event without a subscription or a name");
        return;
    }
    QJsonValue payload = object.value(QStringLiteral("payload"));
    if (payload.isUndefined()) {
        payload = QJsonValue();
    }
    const auto route = Routes_.find(*subscription);
    if (route != Routes_.end()) {
        Deliver(route->second, name.toString(), payload);
    } else if (Pending_ > 0 && Parked_.size() < MAX_PARKED) {
        // The event overtook the answer that names its subscription; or its
        // subscription is already gone, and the park is cleared with the
        // last pending answer.
        Parked_.push_back(Parked{*subscription, name.toString(), payload});
    }
}

void BusService::OnSubscribed(const QString& pattern,
                              const rpc::Result& answer) {
    const auto remote = Remotes_.find(pattern);
    if (remote == Remotes_.end()) {
        return;
    }
    --Pending_;
    const std::optional<qint64> subscription =
        answer ? rpc::ToInteger(
                     answer->toObject().value(QStringLiteral("subscription")))
               : std::nullopt;
    std::vector<Parked> mine;
    if (!subscription) {
        qWarning("bus.subscribe %s failed: %s; its listeners are dropped",
                 qUtf8Printable(pattern),
                 answer ? "no subscription in the answer"
                        : qUtf8Printable(answer.error().Message));
        for (const LocalId id : std::exchange(remote->second.Locals, {})) {
            Forget(id);
        }
        Remotes_.erase(remote);
    } else if (remote->second.Locals.empty()) {
        // Everybody left while the answer was on its way.
        Remotes_.erase(remote);
        SendUnsubscribe(*subscription);
    } else {
        remote->second.Subscription = subscription;
        Routes_.insert_or_assign(*subscription, pattern);
        const auto split = std::stable_partition(
            Parked_.begin(), Parked_.end(), [&](const Parked& event) {
                return event.Subscription != *subscription;
            });
        mine.assign(std::make_move_iterator(split),
                    std::make_move_iterator(Parked_.end()));
        Parked_.erase(split, Parked_.end());
    }
    if (Pending_ == 0) {
        Parked_.clear();
    }
    for (const Parked& event : mine) {
        Deliver(pattern, event.Name, event.Payload);
    }
}

void BusService::OnDisconnected() {
    ++Epoch_;
    Remotes_.clear();
    Routes_.clear();
    Parked_.clear();
    Pending_ = 0;
}

void BusService::Deliver(const QString& pattern, const QString& name,
                         const QJsonValue& payload) {
    const auto remote = Remotes_.find(pattern);
    if (remote == Remotes_.end()) {
        return;
    }
    // A listener may unsubscribe, itself or another, so look each one up.
    const std::vector<LocalId> listeners = remote->second.Locals;
    for (const LocalId id : listeners) {
        const auto local = Locals_.find(id);
        if (local == Locals_.end()) {
            continue;
        }
        const std::shared_ptr<const Listener> call = local->second.Call;
        (*call)(name, payload);
    }
}

// Removes the listener only; the pattern's Remote is the caller's business.
void BusService::Forget(LocalId id) {
    const auto local = Locals_.find(id);
    if (local == Locals_.end()) {
        return;
    }
    QObject::disconnect(local->second.Guard);
    Locals_.erase(local);
}

void BusService::Drop(LocalId id) {
    const auto local = Locals_.find(id);
    if (local == Locals_.end()) {
        return;
    }
    const auto remote = Remotes_.find(local->second.Pattern);
    Forget(id);
    // No Remote, or one of a later connection that does not know the id: the
    // listener was left over from a lost connection.
    if (remote == Remotes_.end() ||
        std::erase(remote->second.Locals, id) == 0) {
        return;
    }
    // While the answer is pending the Remote stays: OnSubscribed ends it, or
    // a new listener of the pattern takes it over.
    if (remote->second.Locals.empty() && remote->second.Subscription) {
        const qint64 subscription = *remote->second.Subscription;
        Routes_.erase(subscription);
        Remotes_.erase(remote);
        SendUnsubscribe(subscription);
    }
}

void BusService::SendUnsubscribe(qint64 subscription) {
    // Nothing to do about a failure: a lost connection took the subscription
    // with it.
    Io_.Request(QStringLiteral("bus.unsubscribe"),
                QJsonObject{{QStringLiteral("subscription"), subscription}});
}

}  // namespace roost::api
