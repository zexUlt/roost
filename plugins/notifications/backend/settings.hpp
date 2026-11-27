#pragma once

#include <QJsonValue>
#include <QString>

#include <optional>
#include <set>
#include <vector>

namespace roost::notifications {

enum class EEvent { Blocked, Done, Failed };

// "blocked", "done" or "failed"; nothing for any other text.
std::optional<EEvent> EventFromName(const QString& name);
QString NameOf(EEvent event);

using EventSet = std::set<EEvent>;

struct CommandSpec {
    QString Name;
    // The text of `bash -c`.
    QString Run;
    // Nothing: every event.
    std::optional<EventSet> Events;
    bool Enabled = true;
};

// The `settings` key of the document of the plugin.
struct Settings {
    // Whether notifications.show is published for the windows.
    bool Desktop = true;
    EventSet Events{EEvent::Blocked, EEvent::Failed};
    std::vector<CommandSpec> Commands;
};

// Reads the `settings` value of the document; undefined or null is the
// defaults. A field of the wrong type, an unknown event and a command that is
// not usable (no name, no text to run, a name taken before) are skipped, each
// with one warning, and the rest is used.
Settings ParseSettings(const QJsonValue& value);

}  // namespace roost::notifications
