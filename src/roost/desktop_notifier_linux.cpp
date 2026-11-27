#include "roost/desktop_notifier_linux.hpp"

#include <QCoreApplication>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFileInfo>
#include <QVariantMap>

#include <utility>

namespace roost {

namespace {

const QString SERVICE = QStringLiteral("org.freedesktop.Notifications");
const QString PATH = QStringLiteral("/org/freedesktop/Notifications");
const QString INTERFACE = QStringLiteral("org.freedesktop.Notifications");
const QString APP_NAME = QStringLiteral("roost");
const QString DEFAULT_ACTION = QStringLiteral("default");
// The icon of the theme, when the file is not next to the executable.
const QString THEME_ICON = QStringLiteral("roost");

}  // namespace

QString NotificationIcon(const QString& applicationDir) {
    const QFileInfo file(applicationDir +
                         QStringLiteral("/../share/icons/hicolor/128x128/apps/"
                                        "roost.png"));
    return file.isFile() ? file.canonicalFilePath() : THEME_ICON;
}

QVariantList NotifyArguments(const QString& title, const QString& body,
                             const QString& icon) {
    return {APP_NAME,      QVariant::fromValue<quint32>(0),
            icon,          title,
            body,          QStringList{DEFAULT_ACTION, QStringLiteral("Open")},
            QVariantMap{}, QVariant::fromValue<qint32>(-1)};
}

LinuxDesktopNotifier::LinuxDesktopNotifier(Connect connect)
    : Connect_(std::move(connect)), Bus_(QString()) {}

QDBusConnection LinuxDesktopNotifier::SessionBus() {
    return QDBusConnection::sessionBus();
}

void LinuxDesktopNotifier::Warn(const QString& message) {
    if (!std::exchange(Warned_, true)) {
        qWarning("roost: no desktop notifications: %s",
                 qUtf8Printable(message));
    }
}

bool LinuxDesktopNotifier::EnsureReady() {
    if (std::exchange(Tried_, true)) {
        return Ready_;
    }
    Bus_ = Connect_();
    if (!Bus_.isConnected()) {
        Warn(QStringLiteral("no session bus"));
        return false;
    }
    const bool connected =
        Bus_.connect(SERVICE, PATH, INTERFACE, QStringLiteral("ActionInvoked"),
                     this, SLOT(OnActionInvoked(uint, QString))) &&
        Bus_.connect(SERVICE, PATH, INTERFACE,
                     QStringLiteral("NotificationClosed"), this,
                     SLOT(OnClosed(uint, uint)));
    if (!connected) {
        Warn(QStringLiteral("cannot listen to %1").arg(SERVICE));
        return false;
    }
    Ready_ = true;
    return true;
}

void LinuxDesktopNotifier::Show(const QString& id, const QString& title,
                                const QString& body) {
    if (!EnsureReady()) {
        return;
    }
    QDBusMessage message = QDBusMessage::createMethodCall(
        SERVICE, PATH, INTERFACE, QStringLiteral("Notify"));
    message.setArguments(NotifyArguments(
        title, body, NotificationIcon(QCoreApplication::applicationDirPath())));
    auto* watcher = new QDBusPendingCallWatcher(Bus_.asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, id](QDBusPendingCallWatcher* done) {
                done->deleteLater();
                const QDBusPendingReply<uint> reply = *done;
                if (reply.isError()) {
                    Warn(reply.error().message());
                    return;
                }
                Ids_.insert(reply.value(), id);
            });
}

void LinuxDesktopNotifier::OnActionInvoked(uint dbusId, const QString& action) {
    const auto shown = Ids_.constFind(dbusId);
    if (action == DEFAULT_ACTION && shown != Ids_.constEnd()) {
        emit Activated(*shown);
    }
}

void LinuxDesktopNotifier::OnClosed(uint dbusId, uint /*reason*/) {
    Ids_.remove(dbusId);
}

}  // namespace roost
