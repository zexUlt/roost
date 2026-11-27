#include "roost/window_notifications.hpp"

#include <QGuiApplication>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QUuid>
#include <QtLogging>

#include <utility>

#include "qml_api/terminal_item.hpp"

namespace roost {

namespace {

const QString SHOW_EVENT = QStringLiteral("notifications.show");
const QString FOCUSED_EVENT = QStringLiteral("shell.windowFocused");
const QString OPEN_COMMAND = QStringLiteral("sessions.open");
// The notifications that name a session and are not clicked: the oldest are
// forgotten past this many.
constexpr std::size_t MAX_TARGETS = 100;

// Whether `item` or an item below it is a terminal that shows the session. The
// terminal is the one the shell reads the session of its panel from
// (roost/shell.cpp, OpeningOf): its `session` is the SessionRef the panel is
// linked to; an item that is not visible shows nothing.
bool ShowsSession(const QQuickItem& item, const QString& agent,
                  const QString& session) {
    if (!item.isVisible()) {
        return false;
    }
    if (const auto* terminal = qobject_cast<const TerminalItem*>(&item)) {
        const QVariantMap ref = terminal->sessionRef().toMap();
        if (ref.value(QStringLiteral("agent")).toString() == agent &&
            ref.value(QStringLiteral("id")).toString() == session) {
            return true;
        }
    }
    for (const QQuickItem* child : item.childItems()) {
        if (ShowsSession(*child, agent, session)) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool WindowNotifications::IsShown(const QString& agent,
                                  const QString& session) const {
    const auto* quick = qobject_cast<const QQuickWindow*>(Window_.data());
    return quick != nullptr && !agent.isEmpty() && !session.isEmpty() &&
           ShowsSession(*quick->contentItem(), agent, session);
}

WindowNotifications::WindowNotifications(api::Api& api,
                                         DesktopNotifier& notifier,
                                         QWindow& window, QObject* parent)
    : QObject(parent),
      Api_(api),
      Notifier_(notifier),
      Window_(&window),
      WindowId_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    const auto shown = Api_.Bus().Subscribe(
        SHOW_EVENT, this,
        [this](const QString& /*name*/, const QJsonValue& payload) {
            OnShow(payload);
        });
    const auto focused = Api_.Bus().Subscribe(
        FOCUSED_EVENT, this,
        [this](const QString& /*name*/, const QJsonValue& payload) {
            LastFocused_ =
                payload.toObject().value(QStringLiteral("window")).toString();
        });
    if (!shown || !focused) {
        qWarning("roost: desktop notifications are off: cannot subscribe");
    }
    connect(qGuiApp, &QGuiApplication::focusWindowChanged, this,
            &WindowNotifications::OnFocusWindowChanged);
    connect(&Api_, &api::Api::Connected, this, [this] {
        const bool wasLast = std::exchange(WasLast_, false);
        if (Focused_ || wasLast) {
            PublishFocus();
        }
    });
    connect(&Api_, &api::Api::Disconnected, this, [this] {
        WasLast_ = IsLastFocused();
        LastFocused_.clear();
    });
    connect(&Notifier_, &DesktopNotifier::Activated, this,
            &WindowNotifications::OnActivated);
    OnFocusWindowChanged(QGuiApplication::focusWindow());
}

void WindowNotifications::OnFocusWindowChanged(QWindow* focused) {
    const bool mine = focused != nullptr && focused == Window_.data();
    if (std::exchange(Focused_, mine) != mine && mine) {
        PublishFocus();
    }
}

void WindowNotifications::PublishFocus() {
    // Not connected yet: the Connected of the api says it again.
    Api_.Bus()
        .Publish(FOCUSED_EVENT,
                 QJsonObject{{QStringLiteral("window"), WindowId_}})
        .then(this, [](const rpc::Result& /*answer*/) {});
}

void WindowNotifications::OnShow(const QJsonValue& payload) {
    if (!IsLastFocused()) {
        return;
    }
    const QJsonObject shown = payload.toObject();
    const QString agent = shown.value(QStringLiteral("agent")).toString();
    const QString session = shown.value(QStringLiteral("session")).toString();
    // The user is looking at that session in this window: nothing to tell.
    if (Focused_ && IsShown(agent, session)) {
        return;
    }
    const quint64 id = NextId_++;
    if (!agent.isEmpty() && !session.isEmpty()) {
        Targets_.emplace(id, Target{.Agent = agent, .Session = session});
        while (Targets_.size() > MAX_TARGETS) {
            Targets_.erase(Targets_.begin());
        }
    }
    Notifier_.Show(QString::number(id),
                   shown.value(QStringLiteral("title")).toString(),
                   shown.value(QStringLiteral("body")).toString());
}

void WindowNotifications::OnActivated(const QString& id) {
    Raise();
    const auto target = Targets_.find(id.toULongLong());
    if (target == Targets_.end()) {
        return;
    }
    const Target opened = target->second;
    Targets_.erase(target);
    OpenSession(opened);
}

void WindowNotifications::Raise() {
    if (Window_) {
        Window_->raise();
        Window_->requestActivate();
    }
}

void WindowNotifications::OpenSession(const Target& target) {
    Api_.Commands()
        .Run(OPEN_COMMAND, QJsonObject{{QStringLiteral("agent"), target.Agent},
                                       {QStringLiteral("id"), target.Session}})
        .then(this, [](const rpc::Result& answer) {
            if (!answer) {
                qWarning("roost: cannot open the session of a notification: %s",
                         qUtf8Printable(answer.error().Message));
            }
        });
}

}  // namespace roost
