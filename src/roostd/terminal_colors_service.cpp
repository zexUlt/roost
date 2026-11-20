#include "roostd/terminal_colors_service.hpp"

#include <QJsonValue>
#include <QtLogging>

#include "core/theme_source.hpp"
#include "roostd/warm_pool.hpp"

namespace roost {

namespace {

const QString SETTINGS = QStringLiteral("settings");

}  // namespace

TerminalColorsService::TerminalColorsService(Store store, Server& server,
                                             WarmPool& pool, QObject* parent)
    : QObject(parent), Server_(server), Pool_(pool) {
    const auto settings = store.Load(SETTINGS);
    if (!settings) {
        qWarning("roostd: settings: %s; the default theme is used",
                 qUtf8Printable(settings.error()));
    }
    Apply(settings.value_or(QJsonObject()));

    // The listener stays with the bus; the destructor takes it away.
    const auto subscription = Server_.Bus().Subscribe(
        QStringLiteral("store.changed"),
        [this](const QString& /*name*/, const QJsonValue& payload) {
            const QJsonObject changed = payload.toObject();
            if (changed.value(QStringLiteral("plugin")).toString() ==
                SETTINGS) {
                Apply(changed.value(QStringLiteral("document")).toObject());
            }
        });
    Q_ASSERT(subscription);
    Subscription_ = subscription.value_or(0);
}

TerminalColorsService::~TerminalColorsService() {
    if (Subscription_ != 0) {
        Server_.Bus().Unsubscribe(Subscription_);
    }
}

void TerminalColorsService::Apply(const QJsonObject& settings) {
    ResolvedTheme theme = ResolveTheme(settings);
    const ResolvedTerminalColors resolved = ResolveTerminalColors(theme.Tokens);
    if (resolved.Fallback) {
        theme.Problem = QStringLiteral("theme \"%1\": %2; the default is used")
                            .arg(theme.Id, resolved.Problem);
    }
    if (theme.Problem != Logged_) {
        Logged_ = theme.Problem;
        if (!Logged_.isEmpty()) {
            qWarning("roostd: %s", qUtf8Printable(Logged_));
        }
    }
    if (!resolved.Colors) {
        // Not even the default theme: the terminals keep what they have.
        qWarning("roostd: the default theme has no terminal colors: %s",
                 qUtf8Printable(resolved.Problem));
        return;
    }
    if (Applied_ == *resolved.Colors) {
        return;
    }
    qInfo("roostd: terminal theme \"%s\"", qUtf8Printable(theme.Id));
    Applied_ = *resolved.Colors;
    Pool_.SetColors(*resolved.Colors);
}

}  // namespace roost
