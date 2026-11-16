#include "roostd/manifest.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

#include <algorithm>
#include <optional>
#include <utility>

#include "core/bus.hpp"
#include "core/store.hpp"

namespace roost {

namespace {

// A manifest is a few kilobytes; this only keeps a stray file from being read
// whole.
constexpr qint64 MAX_MANIFEST_SIZE = 1024 * 1024;

const QString MANIFEST_FILE = QStringLiteral("plugin.json");

using Checked = std::expected<void, QString>;

Checked Fail(const QString& message) { return std::unexpected(message); }

bool IsNonEmptyString(const QJsonValue& value) {
    return value.isString() && !value.toString().isEmpty();
}

// Command ids follow the event name grammar: dot-separated segments of
// letters, digits, '_' and '-', such as "claude.newSession".
bool IsValidCommandId(const QJsonValue& value) {
    return value.isString() && EventBus::IsValidName(value.toString());
}

// The path a manifest names, resolved, if it stays inside the plugin
// directory. The check is on the text: a symlink is not followed, as the
// plugin's own code runs as the user anyway.
std::optional<QString> InsidePlugin(const QString& directory,
                                    const QString& path) {
    const QString resolved = QDir::cleanPath(directory + u'/' + path);
    if (QDir::isAbsolutePath(path) || !resolved.startsWith(directory + u'/')) {
        return std::nullopt;
    }
    return resolved;
}

Checked ParseBackend(const QJsonValue& value, PluginManifest& manifest) {
    if (value.isUndefined()) {
        return {};
    }
    const QJsonArray command = value.toObject().value("command").toArray();
    if (!value.isObject() || command.isEmpty()) {
        return Fail(QStringLiteral(
            "\"backend\" must be {\"command\": [program, arguments...]}"));
    }
    for (const QJsonValue& part : command) {
        if (!part.isString()) {
            return Fail(
                QStringLiteral("\"backend.command\" must hold strings"));
        }
        manifest.Backend.append(part.toString());
    }
    const QString program = manifest.Backend.front();
    const std::optional<QString> resolved =
        InsidePlugin(manifest.Directory, program);
    if (!resolved) {
        return Fail(QStringLiteral("backend program \"%1\" must be a path "
                                   "inside the plugin directory")
                        .arg(program));
    }
    manifest.Backend.front() = *resolved;
    return {};
}

Checked ParseCommands(const QJsonValue& value, PluginManifest& manifest) {
    if (value.isUndefined()) {
        return {};
    }
    if (!value.isArray()) {
        return Fail(
            QStringLiteral("\"contributes.commands\" must be an array"));
    }
    for (const QJsonValue& entry : value.toArray()) {
        const QJsonObject command = entry.toObject();
        const QJsonValue id = command.value("id");
        const QJsonValue title = command.value("title");
        if (!IsValidCommandId(id) || !IsNonEmptyString(title)) {
            return Fail(QStringLiteral(
                "a command needs an id such as \"plugin.doThing\" and a "
                "title"));
        }
        manifest.Commands.append(
            {.Id = id.toString(), .Title = title.toString()});
    }
    return {};
}

Checked ParseKeybindings(const QJsonValue& value, PluginManifest& manifest) {
    if (value.isUndefined()) {
        return {};
    }
    if (!value.isArray()) {
        return Fail(
            QStringLiteral("\"contributes.keybindings\" must be an array"));
    }
    for (const QJsonValue& entry : value.toArray()) {
        const QJsonObject binding = entry.toObject();
        const QJsonValue command = binding.value("command");
        const QJsonValue key = binding.value("key");
        if (!IsValidCommandId(command) || !key.isString() ||
            key.toString().trimmed().isEmpty()) {
            return Fail(
                QStringLiteral("a keybinding needs a command id and a key"));
        }
        const bool declared = std::ranges::any_of(
            manifest.Commands, [&](const PluginCommand& own) {
                return own.Id == command.toString();
            });
        if (!declared) {
            return Fail(QStringLiteral("key %1 is bound to %2, a command the "
                                       "plugin does not declare")
                            .arg(key.toString(), command.toString()));
        }
        manifest.Keybindings.append(
            {.Command = command.toString(), .Key = key.toString()});
    }
    return {};
}

Checked ParseAgents(const QJsonValue& value, PluginManifest& manifest) {
    if (value.isUndefined()) {
        return {};
    }
    if (!value.isArray()) {
        return Fail(QStringLiteral("\"contributes.agents\" must be an array"));
    }
    for (const QJsonValue& entry : value.toArray()) {
        const QJsonObject agent = entry.toObject();
        const QJsonValue id = agent.value("id");
        if (!id.isString() || !Store::IsValidPluginId(id.toString())) {
            return Fail(QStringLiteral(
                "an agent needs an id of lowercase letters, digits, '.', '_' "
                "and '-'"));
        }
        const QJsonValue capabilities = agent.value("capabilities");
        if (!capabilities.isUndefined() && !capabilities.isArray()) {
            return Fail(
                QStringLiteral("the capabilities of agent %1 must be an array")
                    .arg(id.toString()));
        }
        PluginAgent parsed{.Id = id.toString(), .Capabilities = {}};
        for (const QJsonValue& capability : capabilities.toArray()) {
            if (!IsNonEmptyString(capability)) {
                return Fail(QStringLiteral("the capabilities of agent %1 must "
                                           "be strings")
                                .arg(id.toString()));
            }
            parsed.Capabilities.append(capability.toString());
        }
        manifest.Agents.append(std::move(parsed));
    }
    if (!manifest.Agents.isEmpty() && manifest.Backend.isEmpty()) {
        return Fail(QStringLiteral(
            "a plugin with agents needs a backend to serve them"));
    }
    return {};
}

// The parts only the window reads are checked for their JSON type here, so a
// broken manifest fails at load rather than when a panel opens.
Checked CheckWindowParts(const QJsonObject& contributes,
                         const QString& directory) {
    const QJsonValue panels = contributes.value("panels");
    if (!panels.isUndefined() && !panels.isArray()) {
        return Fail(QStringLiteral("\"contributes.panels\" must be an array"));
    }
    const QJsonValue settings = contributes.value("settings");
    if (!settings.isUndefined() &&
        (!settings.isString() ||
         !InsidePlugin(directory, settings.toString()))) {
        return Fail(
            QStringLiteral("\"contributes.settings\" must be a path "
                           "inside the plugin directory"));
    }
    return {};
}

}  // namespace

std::expected<PluginManifest, QString> ParseManifest(const QByteArray& json,
                                                     const QString& directory) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError) {
        return std::unexpected(QStringLiteral("not valid JSON: %1 at offset %2")
                                   .arg(error.errorString())
                                   .arg(error.offset));
    }
    if (!document.isObject()) {
        return std::unexpected(QStringLiteral("not a JSON object"));
    }
    if (!QDir::isAbsolutePath(directory)) {
        return std::unexpected(
            QStringLiteral("plugin directory \"%1\" is not absolute")
                .arg(directory));
    }
    const QJsonObject root = document.object();

    PluginManifest manifest;
    manifest.Directory = QDir::cleanPath(directory);
    const QJsonValue id = root.value("id");
    if (!id.isString() || !Store::IsValidPluginId(id.toString())) {
        return std::unexpected(QStringLiteral(
            "\"id\" must be lowercase letters, digits, '.', '_' and '-'"));
    }
    if (Store::IsReservedPluginId(id.toString())) {
        return std::unexpected(
            QStringLiteral("\"id\" \"%1\" is reserved: the state directory "
                           "keeps the user's file of that name")
                .arg(id.toString()));
    }
    manifest.Id = id.toString();
    const QJsonValue version = root.value("version");
    if (!IsNonEmptyString(version)) {
        return std::unexpected(QStringLiteral("\"version\" must be a string"));
    }
    manifest.Version = version.toString();
    const QJsonValue api = root.value("api");
    if (api != QJsonValue(PLUGIN_API)) {
        return std::unexpected(
            QStringLiteral("\"api\" must be \"%1\", the API roostd serves")
                .arg(PLUGIN_API));
    }

    const QJsonValue contributes = root.value("contributes");
    if (!contributes.isUndefined() && !contributes.isObject()) {
        return std::unexpected(
            QStringLiteral("\"contributes\" must be an object"));
    }
    manifest.Contributes = contributes.toObject();

    if (auto backend = ParseBackend(root.value("backend"), manifest);
        !backend) {
        return std::unexpected(backend.error());
    }
    if (auto commands =
            ParseCommands(manifest.Contributes.value("commands"), manifest);
        !commands) {
        return std::unexpected(commands.error());
    }
    if (auto keybindings = ParseKeybindings(
            manifest.Contributes.value("keybindings"), manifest);
        !keybindings) {
        return std::unexpected(keybindings.error());
    }
    if (auto agents =
            ParseAgents(manifest.Contributes.value("agents"), manifest);
        !agents) {
        return std::unexpected(agents.error());
    }
    if (auto window =
            CheckWindowParts(manifest.Contributes, manifest.Directory);
        !window) {
        return std::unexpected(window.error());
    }
    return manifest;
}

std::expected<PluginManifest, QString> LoadManifest(const QString& directory) {
    const QString path = directory + u'/' + MANIFEST_FILE;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(
            QStringLiteral("cannot read %1: %2").arg(path, file.errorString()));
    }
    if (file.size() > MAX_MANIFEST_SIZE) {
        return std::unexpected(QStringLiteral("%1 is larger than %2 bytes")
                                   .arg(path)
                                   .arg(MAX_MANIFEST_SIZE));
    }
    auto manifest = ParseManifest(file.readAll(), directory);
    if (!manifest) {
        return std::unexpected(path + QStringLiteral(": ") + manifest.error());
    }
    return manifest;
}

QStringList PluginSearchPath(const QString& configDirectory) {
    QStringList path;
    for (const QString& entry :
         qEnvironmentVariable("ROOST_PLUGINS_PATH")
             .split(QDir::listSeparator(), Qt::SkipEmptyParts)) {
        if (QDir::isAbsolutePath(entry)) {
            path.append(QDir::cleanPath(entry));
        }
    }
    path.append(QDir::cleanPath(configDirectory + QStringLiteral("/plugins")));
    // A directory searched twice would hold every plugin twice.
    path.removeDuplicates();
    return path;
}

QStringList FindPluginDirectories(const QStringList& searchPath) {
    QStringList found;
    for (const QString& root : searchPath) {
        const QDir directory(root);
        for (const QString& name : directory.entryList(
                 QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QString candidate = directory.filePath(name);
            if (QFileInfo(candidate + u'/' + MANIFEST_FILE).isFile()) {
                found.append(candidate);
            }
        }
    }
    return found;
}

bool PluginRegistry::Add(PluginManifest manifest) {
    if (const PluginManifest* loaded = Find(manifest.Id)) {
        Problems_.append(
            QStringLiteral("plugin %1 in %2 is not loaded: the one in %3 "
                           "came first")
                .arg(manifest.Id, manifest.Directory, loaded->Directory));
        return false;
    }
    const std::size_t index = Plugins_.size();
    // The owner of a taken name, for the message: an earlier plugin, or this
    // one if it declares the name twice.
    const auto ownerOf = [&](std::size_t owner) {
        return owner == index ? manifest.Id : Plugins_[owner].Id;
    };
    for (const PluginCommand& command : manifest.Commands) {
        if (const auto taken = Commands_.constFind(command.Id);
            taken != Commands_.cend()) {
            Problems_.append(
                QStringLiteral("command %1 of plugin %2 is ignored: plugin %3 "
                               "has it")
                    .arg(command.Id, manifest.Id, ownerOf(*taken)));
            continue;
        }
        Commands_.insert(command.Id, index);
    }
    for (const PluginKeybinding& binding : manifest.Keybindings) {
        // A key runs the command of whoever owns it, so a plugin binds only
        // the commands it kept.
        if (const auto command = Commands_.constFind(binding.Command);
            command == Commands_.cend() || *command != index) {
            Problems_.append(
                command == Commands_.cend()
                    ? QStringLiteral("key %1 of plugin %2 is ignored: the "
                                     "plugin has no command %3")
                          .arg(binding.Key, manifest.Id, binding.Command)
                    : QStringLiteral("key %1 of plugin %2 is ignored: its "
                                     "command %3 belongs to plugin %4")
                          .arg(binding.Key, manifest.Id, binding.Command,
                               ownerOf(*command)));
            continue;
        }
        const QString key = binding.Key.toLower();
        if (const auto taken = Keys_.constFind(key); taken != Keys_.cend()) {
            Problems_.append(
                QStringLiteral("key %1 of plugin %2 is ignored: plugin %3 "
                               "binds it")
                    .arg(binding.Key, manifest.Id, ownerOf(*taken)));
            continue;
        }
        Keys_.insert(key, index);
    }
    for (const PluginAgent& agent : manifest.Agents) {
        if (const auto taken = Agents_.constFind(agent.Id);
            taken != Agents_.cend()) {
            Problems_.append(
                QStringLiteral("agent %1 of plugin %2 is ignored: plugin %3 "
                               "serves it")
                    .arg(agent.Id, manifest.Id, ownerOf(*taken)));
            continue;
        }
        Agents_.insert(agent.Id, index);
    }
    ById_.insert(manifest.Id, index);
    Plugins_.push_back(std::move(manifest));
    return true;
}

void PluginRegistry::AddProblem(QString problem) {
    Problems_.append(std::move(problem));
}

const PluginManifest* PluginRegistry::Find(const QString& pluginId) const {
    const auto it = ById_.constFind(pluginId);
    return it == ById_.cend() ? nullptr : &Plugins_[*it];
}

const PluginManifest* PluginRegistry::CommandOwner(
    const QString& commandId) const {
    const auto it = Commands_.constFind(commandId);
    return it == Commands_.cend() ? nullptr : &Plugins_[*it];
}

const PluginManifest* PluginRegistry::AgentOwner(const QString& agentId) const {
    const auto it = Agents_.constFind(agentId);
    return it == Agents_.cend() ? nullptr : &Plugins_[*it];
}

const PluginManifest* PluginRegistry::KeyOwner(const QString& key) const {
    const auto it = Keys_.constFind(key.toLower());
    return it == Keys_.cend() ? nullptr : &Plugins_[*it];
}

}  // namespace roost
