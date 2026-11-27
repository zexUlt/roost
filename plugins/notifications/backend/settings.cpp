#include "notifications/backend/settings.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#include "notifications/backend/diagnostics.hpp"

namespace roost::notifications {

std::optional<EEvent> EventFromName(const QString& name) {
    if (name == QStringLiteral("blocked")) {
        return EEvent::Blocked;
    }
    if (name == QStringLiteral("done")) {
        return EEvent::Done;
    }
    if (name == QStringLiteral("failed")) {
        return EEvent::Failed;
    }
    return std::nullopt;
}

QString NameOf(EEvent event) {
    switch (event) {
        case EEvent::Blocked:
            return QStringLiteral("blocked");
        case EEvent::Done:
            return QStringLiteral("done");
        case EEvent::Failed:
            return QStringLiteral("failed");
    }
    return {};
}

namespace {

// The events of an array; nothing, with a warning, if it is not an array of
// names of events. `where` names the field for the warning.
std::optional<EventSet> ParseEvents(const QJsonValue& value,
                                    const QString& where) {
    if (!value.isArray()) {
        Warn(
            QStringLiteral("%1 is not an array of events: skipped").arg(where));
        return std::nullopt;
    }
    EventSet events;
    bool unknown = false;
    for (const QJsonValue& item : value.toArray()) {
        const auto event =
            item.isString() ? EventFromName(item.toString()) : std::nullopt;
        if (event) {
            events.insert(*event);
        } else {
            unknown = true;
        }
    }
    if (unknown) {
        Warn(QStringLiteral(
                 "%1 has an entry that is not blocked, done or failed: "
                 "skipped")
                 .arg(where));
    }
    return events;
}

}  // namespace

Settings ParseSettings(const QJsonValue& value) {
    Settings settings;
    if (value.isUndefined() || value.isNull()) {
        return settings;
    }
    if (!value.isObject()) {
        Warn(
            QStringLiteral("settings is not an object: the defaults are used"));
        return settings;
    }
    const QJsonObject object = value.toObject();

    if (const QJsonValue desktop = object.value(QStringLiteral("desktop"));
        desktop.isBool()) {
        settings.Desktop = desktop.toBool();
    } else if (!desktop.isUndefined()) {
        Warn(QStringLiteral("settings.desktop is not a boolean: skipped"));
    }

    if (const QJsonValue events = object.value(QStringLiteral("events"));
        !events.isUndefined()) {
        if (const auto parsed =
                ParseEvents(events, QStringLiteral("settings.events"))) {
            settings.Events = *parsed;
        }
    }

    const QJsonValue commands = object.value(QStringLiteral("commands"));
    if (commands.isUndefined()) {
        return settings;
    }
    if (!commands.isArray()) {
        Warn(QStringLiteral("settings.commands is not an array: skipped"));
        return settings;
    }
    QSet<QString> names;
    int index = 0;
    for (const QJsonValue& item : commands.toArray()) {
        const QString where =
            QStringLiteral("settings.commands[%1]").arg(index);
        ++index;
        const QJsonObject command = item.toObject();
        const QJsonValue name = command.value(QStringLiteral("name"));
        const QJsonValue run = command.value(QStringLiteral("run"));
        if (!item.isObject() || !name.isString() || name.toString().isEmpty() ||
            !run.isString() || run.toString().isEmpty()) {
            Warn(QStringLiteral("%1 has no name or no run, both text: skipped")
                     .arg(where));
            continue;
        }
        if (names.contains(name.toString())) {
            Warn(QStringLiteral("%1: the name %2 is taken by an earlier "
                                "command: skipped")
                     .arg(where, name.toString()));
            continue;
        }
        names.insert(name.toString());
        CommandSpec spec{.Name = name.toString(), .Run = run.toString()};
        if (const QJsonValue events = command.value(QStringLiteral("events"));
            !events.isUndefined()) {
            spec.Events =
                ParseEvents(events, where + QStringLiteral(".events"));
        }
        if (const QJsonValue enabled = command.value(QStringLiteral("enabled"));
            enabled.isBool()) {
            spec.Enabled = enabled.toBool();
        } else if (!enabled.isUndefined()) {
            Warn(QStringLiteral("%1.enabled is not a boolean: skipped")
                     .arg(where));
        }
        settings.Commands.push_back(std::move(spec));
    }
    return settings;
}

}  // namespace roost::notifications
