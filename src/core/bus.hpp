#pragma once

#include <QJsonValue>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace roost {

// The event bus of roostd (design.md §2): plugins and the daemon publish
// named events, and whoever subscribed gets them synchronously, in the order
// they subscribed.
//
// An event name is dot-separated segments of letters, digits, '_' and '-':
// "session.opened". A subscription takes an exact name, a prefix ending in
// ".*" that matches every name under it ("session.*" matches
// "session.opened" and "session.a.b", not "session"), or "*" for everything.
//
// A listener may subscribe, unsubscribe and publish while it is being called.
// A subscription made during a delivery does not get the event being
// delivered, though it does get the events of publishes that start after it;
// one removed during a delivery gets nothing after its removal. A nested
// publish is delivered in full before the outer one goes on, and publishes
// nest at most MAX_PUBLISH_DEPTH deep, so a listener that republishes into its
// own pattern ends instead of overflowing the stack.
//
// An exception from a listener stops that delivery and reaches the caller of
// Publish; the subscriptions are left as they were.
class EventBus {
public:
    using Listener =
        std::function<void(const QString& name, const QJsonValue& payload)>;
    using SubscriptionId = std::uint64_t;

    static constexpr int MAX_PUBLISH_DEPTH = 16;

    static bool IsValidName(const QString& name);
    static bool IsValidPattern(const QString& pattern);

    // Nothing if the pattern is not valid.
    std::optional<SubscriptionId> Subscribe(const QString& pattern,
                                            Listener listener);

    // False if there is no such subscription, or it is already gone.
    bool Unsubscribe(SubscriptionId id);

    // False, and nothing delivered, if the name is not valid or the publish
    // would nest deeper than MAX_PUBLISH_DEPTH.
    bool Publish(const QString& name, const QJsonValue& payload);

    std::size_t SubscriptionCount() const { return Subscriptions_.size(); }

private:
    struct Subscription {
        QString Pattern;
        // Shared so that a listener that unsubscribes itself is not destroyed
        // while it runs.
        std::shared_ptr<const Listener> Call;
    };

    static bool Matches(const QString& pattern, const QString& name);

    std::map<SubscriptionId, Subscription> Subscriptions_;
    SubscriptionId NextId_ = 1;
    int Depth_ = 0;
};

}  // namespace roost
