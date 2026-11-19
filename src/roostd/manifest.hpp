#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <expected>
#include <optional>
#include <vector>

namespace roost {

// The plugin API roostd serves (design.md §5). Version 0 is unstable: a
// plugin states the one it was written for and must match exactly.
inline const QString PLUGIN_API = QStringLiteral("0");

struct PluginCommand {
    QString Id;
    QString Title;
};

struct PluginKeybinding {
    QString Command;
    QString Key;
};

// An agent the plugin's backend serves (design.md §7).
struct PluginAgent {
    QString Id;
    // As declared, such as "nativeTuiAttach"; names roostd does not know
    // are kept.
    QStringList Capabilities;
};

// The part of the "native" key that roost loads today: the QML module of the
// plugin. "library" and "sdk" come with the SDK (13-plan.md, M6) and are
// refused until then.
struct PluginNative {
    // As declared, "6.N"; empty if the manifest leaves it out.
    QString Qt;
    // As declared, relative to the plugin directory; empty if left out.
    QString QmlImportPath;
};

// plugin.json, checked. What the daemon acts on is parsed out; the rest of
// "contributes" (panels, settings) stays as JSON for the window.
struct PluginManifest {
    QString Id;
    QString Version;
    QString Directory;  // absolute; relative paths in the manifest start here
    // The backend's program and arguments; empty if the plugin has none. The
    // program is an absolute path inside Directory.
    QStringList Backend;
    QList<PluginCommand> Commands;
    QList<PluginKeybinding> Keybindings;
    QList<PluginAgent> Agents;
    // Absent when the manifest has no "native".
    std::optional<PluginNative> Native;
    QJsonObject Contributes;
};

// Checks a manifest read from `directory`, which must be absolute. Unknown
// keys are ignored, so an older roostd reads a newer manifest of the same
// API. The backend program and the settings page are paths relative to the
// plugin directory that stay inside it, as written: symlinks are not
// followed. "native.qt" is a Qt of the major version roost is built with and
// a minor not newer than it, as Qt loads no newer one. A keybinding names a
// command of the same plugin. A plugin with agents has a backend to serve them.
std::expected<PluginManifest, QString> ParseManifest(const QByteArray& json,
                                                     const QString& directory);

// Reads and checks <directory>/plugin.json.
std::expected<PluginManifest, QString> LoadManifest(const QString& directory);

// The directory with the built-in plugins of the package that roostd runs
// from, given the directory of the roostd binary: the first that exists of
// <dir>/../Resources/roost-plugins (.app), <dir>/../lib/roost/plugins
// (AppImage, FHS) and <dir>/../roost-plugins (build tree). Empty if none does.
QString BundledPluginsDirectory(const QString& applicationDirectory);

// Where plugins are looked for, in load order: each absolute entry of
// ROOST_PLUGINS_PATH (separated by ':'), or `bundledDirectory` if the variable
// is unset, empty or has no absolute entry (an empty `bundledDirectory` adds
// nothing), then
// <config directory>/plugins. Each directory is listed once, where it first
// comes.
QStringList PluginSearchPath(const QString& configDirectory,
                             const QString& bundledDirectory);

// The subdirectories with a plugin.json, of each search directory in turn,
// by name within one, case first: "Zed" comes before "alpha". Missing search
// directories are skipped.
QStringList FindPluginDirectories(const QStringList& searchPath);

// The loaded plugins and who owns each contribution (design.md §5). The
// earlier plugin wins a conflict: a second plugin with a taken id is not
// loaded, and a command id, an agent id or a key that is taken stays with its
// first owner, and so do the keys bound to it. Every refusal is kept as a
// problem for the user to see. Keys are compared ignoring case; their full
// normal form comes with the keymap (M6).
//
// The pointers and the reference it hands out are good until the next Add.
class PluginRegistry {
public:
    // False if the plugin is refused for its id.
    bool Add(PluginManifest manifest);

    // Notes a plugin that did not load at all, such as a broken manifest.
    void AddProblem(QString problem);

    // Null if there is no such plugin.
    const PluginManifest* Find(const QString& pluginId) const;

    // The plugin whose command this is, or null.
    const PluginManifest* CommandOwner(const QString& commandId) const;

    // The plugin that serves this agent, or null.
    const PluginManifest* AgentOwner(const QString& agentId) const;

    // The plugin whose binding of this key counts, or null.
    const PluginManifest* KeyOwner(const QString& key) const;

    // In load order.
    const std::vector<PluginManifest>& Plugins() const { return Plugins_; }

    const QStringList& Problems() const { return Problems_; }

private:
    std::vector<PluginManifest> Plugins_;
    QHash<QString, std::size_t> ById_;
    QHash<QString, std::size_t> Commands_;
    QHash<QString, std::size_t> Keys_;
    QHash<QString, std::size_t> Agents_;
    QStringList Problems_;
};

}  // namespace roost
