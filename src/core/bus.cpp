#include "core/bus.hpp"

#include <QScopeGuard>
#include <QStringView>

#include <utility>

namespace roost {

namespace {

const QString EVERYTHING = QStringLiteral("*");
const QString PREFIX_SUFFIX = QStringLiteral(".*");

bool IsNameChar(QChar c) {
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') ||
           (c >= u'0' && c <= u'9') || c == u'_' || c == u'-';
}

}  // namespace

bool EventBus::IsValidName(const QString& name) {
    bool segmentEmpty = true;
    for (const QChar c : name) {
        if (c == u'.') {
            if (segmentEmpty) {
                return false;
            }
            segmentEmpty = true;
        } else if (IsNameChar(c)) {
            segmentEmpty = false;
        } else {
            return false;
        }
    }
    return !segmentEmpty;
}

bool EventBus::IsValidPattern(const QString& pattern) {
    if (pattern == EVERYTHING) {
        return true;
    }
    if (pattern.endsWith(PREFIX_SUFFIX)) {
        return IsValidName(pattern.chopped(PREFIX_SUFFIX.size()));
    }
    return IsValidName(pattern);
}

bool EventBus::Matches(const QString& pattern, const QString& name) {
    if (pattern == EVERYTHING) {
        return true;
    }
    if (pattern.endsWith(PREFIX_SUFFIX)) {
        // "session.*" keeps its dot, so it matches "session.x" but neither
        // "session" nor "sessions.x". "session." alone is not a valid name.
        return name.startsWith(QStringView(pattern).chopped(1));
    }
    return pattern == name;
}

std::optional<EventBus::SubscriptionId> EventBus::Subscribe(
    const QString& pattern, Listener listener) {
    if (!IsValidPattern(pattern) || !listener) {
        return std::nullopt;
    }
    const SubscriptionId id = NextId_++;
    Subscriptions_.emplace(
        id, Subscription{
                .Pattern = pattern,
                .Call = std::make_shared<const Listener>(std::move(listener)),
            });
    return id;
}

bool EventBus::Unsubscribe(SubscriptionId id) {
    return Subscriptions_.erase(id) > 0;
}

bool EventBus::Publish(const QString& name, const QJsonValue& payload) {
    if (!IsValidName(name) || Depth_ >= MAX_PUBLISH_DEPTH) {
        return false;
    }
    ++Depth_;
    // Undone on every way out, a listener's exception included.
    const auto nesting = qScopeGuard([this] { --Depth_; });
    // Ids only grow, so the subscriptions made by the listeners of this event
    // are exactly those past the last id handed out before it.
    const SubscriptionId last = NextId_ - 1;
    // A listener may change the map, so every step looks up the next id afresh
    // instead of holding an iterator across the call.
    for (auto it = Subscriptions_.begin();
         it != Subscriptions_.end() && it->first <= last;) {
        const SubscriptionId id = it->first;
        if (Matches(it->second.Pattern, name)) {
            const std::shared_ptr<const Listener> call = it->second.Call;
            (*call)(name, payload);
        }
        it = Subscriptions_.upper_bound(id);
    }
    return true;
}

}  // namespace roost
