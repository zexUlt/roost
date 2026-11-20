#pragma once

#include <QString>
#include <QUrl>

namespace roost::sessions {

// The directory of the icons the user keeps: `agent-icons` in the config
// directory, found by the rule of ConfigDirectory in src/core/store.cpp (kept
// here so that the plugin does not link roost core): ROOST_CONFIG, else an
// absolute XDG_CONFIG_HOME/roost, else ~/.config/roost. Empty when none of them
// gives a directory (HOME is not an absolute path or is the root): then only
// the bundled icons are used.
QString UserAgentIconsDirectory();

// Whether `agent` may name a file: [A-Za-z0-9._-]+, and not "." or "..".
bool IsAgentIconName(const QString& agent);

// The icon of an agent: the user's `<agent>.svg`, then `.png`, then the
// bundled `<agent>.svg`, then the user's `default.svg`, then `.png`, else the
// bundled default.svg. An agent that is not a plain file name gets the last
// two. A file URL or a qrc URL.
QUrl ResolveAgentIcon(const QString& agent);

// The bundled icon of an agent, or the bundled default: what to show when the
// file of the user cannot be loaded.
QUrl ResolveBundledAgentIcon(const QString& agent);

}  // namespace roost::sessions
