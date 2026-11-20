#pragma once

#include <QString>
#include <QUrl>

namespace roost::sessions {

// The directory of the icons the user keeps: `agent-icons` in the config
// directory (ROOST_CONFIG, else an absolute XDG_CONFIG_HOME/roost, else
// ~/.config/roost: the rule of ConfigDirectory in src/core/store.cpp, kept here
// so that the plugin does not link roost core).
QString UserAgentIconsDirectory();

// Whether `agent` may name a file: [A-Za-z0-9._-]+, and not "." or "..".
bool IsAgentIconName(const QString& agent);

// The icon of an agent: the user's `<agent>.svg`, then `.png`, then the
// bundled `<agent>.svg`, else the bundled default.svg. An agent that is not a
// plain file name gets the default. A file URL or a qrc URL.
QUrl ResolveAgentIcon(const QString& agent);

}  // namespace roost::sessions
