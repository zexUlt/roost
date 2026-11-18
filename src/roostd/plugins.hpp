#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <unordered_map>
#include <vector>

#include "roostd/adapter_bridge.hpp"
#include "roostd/backend.hpp"
#include "roostd/manifest.hpp"

namespace roost {

class Server;

// The plugins roostd serves (design.md §5): it loads them, keeps their
// backends running, routes commands to them and bridges to the agents they
// serve (§7). It adds to the server:
//
//   commands.run {command, args?}  the answer of the backend of the plugin
//                                  that owns the command, which gets the same
//                                  request
//   sessions.list                  {sessions, unavailable}: the sessions of
//                                  every agent, agent by agent in load order,
//                                  and {agent, reason} for each agent whose
//                                  adapter failed to list them
//   plugins.list                   {plugins, problems}: the loaded plugins in
//                                  load order, each {id, version, directory,
//                                  contributes}, and the reason of each
//                                  plugin that did not load or lost a
//                                  contribution
//
// and publishes on the bus:
//
//   plugin.backendCrashed {plugin, reason, restartInMs}
//   plugin.backendGaveUp  {plugin, reason}
//   session.stateChanged  session, as a backend notifies it with
//                         session.stateChanged {agent, session} about an
//                         agent of its own plugin
//   session.removed       {agent, id}, as a backend notifies it with
//                         session.removed {agent, session: <id>} about an
//                         agent of its own plugin
//   session.adapterStarted {plugin, agents}, when the backend of a plugin
//                         that serves agents started again after it stopped
//                         or failed to start: agents are the ones it owns. Its
//                         sessions may have changed while it was down, with
//                         no event to tell of it, so a listener lists them
//                         again. Under session.* to come after the last
//                         events of the process that is gone: the bus keeps
//                         the order within one pattern only.
//
// The server must outlive it; after it is gone, commands.run fails with
// BackendUnavailable.
class PluginHost : public QObject {
    Q_OBJECT

public:
    explicit PluginHost(Server& server, BackendOptions options = {},
                        QObject* parent = nullptr);
    // Stops the backends.
    ~PluginHost() override;

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // Loads the plugins found on the search path, in its order, and starts
    // their backends. A plugin that cannot load becomes a problem in
    // Registry(). Once per host.
    void Load(const QStringList& searchPath);

    const PluginRegistry& Registry() const { return Registry_; }

    // Null if the plugin has no backend or is not loaded.
    Backend* BackendOf(const QString& pluginId) const;

    // Null if no loaded plugin serves the agent.
    AdapterBridge* BridgeOf(const QString& agentId) const;

private:
    void StartBackend(const PluginManifest& plugin);
    void OnNotified(const QString& pluginId, const QString& method,
                    const QJsonValue& params);
    void OnStateChanged(const QString& pluginId, const QJsonObject& params);
    void OnRemoved(const QString& pluginId, const QJsonObject& params);
    // Whether the plugin is the one that serves the agent.
    bool ServesAgent(const QString& pluginId, const QString& agent) const;

    Server& Server_;
    BackendOptions Options_;
    PluginRegistry Registry_;
    std::unordered_map<QString, std::unique_ptr<Backend>> Backends_;
    // In load order. Declared after the backends, so gone before them.
    std::vector<std::unique_ptr<AdapterBridge>> Bridges_;
    bool Loaded_ = false;
};

}  // namespace roost
