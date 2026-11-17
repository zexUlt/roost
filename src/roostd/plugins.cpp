#include "roostd/plugins.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPointer>

#include "core/protocol.hpp"
#include "roostd/server.hpp"

namespace roost {

namespace {

rpc::Result InvalidParams(const QString& message) {
    return std::unexpected(
        rpc::Error::FromCode(rpc::EErrorCode::InvalidParams, message));
}

rpc::Result Unavailable(const QString& message) {
    return std::unexpected(
        protocol::MakeError(protocol::EErrorCode::BackendUnavailable, message));
}

}  // namespace

PluginHost::PluginHost(Server& server, BackendOptions options, QObject* parent)
    : QObject(parent), Server_(server), Options_(options) {
    // The handler stays with the server; it finds the host gone, if it is.
    const QPointer<PluginHost> self(this);
    const bool registered = Server_.Register(
        QStringLiteral("commands.run"),
        [self](const rpc::Call& call) -> QFuture<rpc::Result> {
            const QJsonObject params = call.Params.toObject();
            const QJsonValue command = params.value(QStringLiteral("command"));
            if (!command.isString()) {
                return rpc::Ready(InvalidParams(QStringLiteral(
                    "commands.run takes {command, args?}: a command id and "
                    "any JSON value")));
            }
            if (!self) {
                return rpc::Ready(
                    Unavailable(QStringLiteral("plugins are not loaded")));
            }
            const PluginManifest* owner =
                self->Registry_.CommandOwner(command.toString());
            if (owner == nullptr) {
                return rpc::Ready(
                    InvalidParams(QStringLiteral("no plugin has command %1")
                                      .arg(command.toString())));
            }
            Backend* backend = self->BackendOf(owner->Id);
            if (backend == nullptr) {
                return rpc::Ready(Unavailable(
                    QStringLiteral("plugin %1 has no backend").arg(owner->Id)));
            }
            QJsonObject request{{QStringLiteral("command"), command}};
            const QJsonValue args = params.value(QStringLiteral("args"));
            if (!args.isUndefined()) {
                request.insert(QStringLiteral("args"), args);
            }
            return backend->Request(QStringLiteral("commands.run"), request);
        });
    Q_ASSERT(registered);
    const bool listing = Server_.Register(
        QStringLiteral("sessions.list"),
        [self](const rpc::Call& /*call*/) -> QFuture<rpc::Result> {
            if (!self) {
                return rpc::Ready(
                    Unavailable(QStringLiteral("plugins are not loaded")));
            }
            using Listed = AdapterBridge::Answer<std::vector<AgentSession>>;
            QStringList agents;
            QList<QFuture<Listed>> listed;
            for (const auto& bridge : self->Bridges_) {
                agents.append(bridge->Agent());
                listed.append(bridge->ListSessions());
            }
            if (listed.isEmpty()) {
                return rpc::Ready(QJsonObject{
                    {QStringLiteral("sessions"), QJsonArray()},
                    {QStringLiteral("unavailable"), QJsonArray()},
                });
            }
            return QtFuture::whenAll(listed.begin(), listed.end())
                .then(QtFuture::Launch::Sync,
                      [agents](
                          const QList<QFuture<Listed>>& done) -> rpc::Result {
                          QJsonArray sessions;
                          QJsonArray unavailable;
                          for (qsizetype i = 0; i < done.size(); ++i) {
                              const Listed answer = done[i].result();
                              if (!answer) {
                                  const rpc::Error& error = answer.error();
                                  unavailable.append(QJsonObject{
                                      {QStringLiteral("agent"), agents[i]},
                                      {QStringLiteral("reason"),
                                       error.Message.isEmpty()
                                           ? QStringLiteral("error %1")
                                                 .arg(error.Code)
                                           : error.Message},
                                  });
                                  continue;
                              }
                              for (const AgentSession& session : *answer) {
                                  sessions.append(ToJson(session));
                              }
                          }
                          const QJsonObject list{
                              {QStringLiteral("sessions"), sessions},
                              {QStringLiteral("unavailable"), unavailable},
                          };
                          // Refused rather than closing the connection.
                          if (!Server::AnswerFitsAFrame(list)) {
                              return std::unexpected(rpc::Error::FromCode(
                                  rpc::EErrorCode::InternalError,
                                  QStringLiteral("the session list does not "
                                                 "fit a frame")));
                          }
                          return list;
                      });
        });
    Q_ASSERT(listing);
    const bool plugins = Server_.Register(
        QStringLiteral("plugins.list"),
        [self](const rpc::Call& /*call*/) -> QFuture<rpc::Result> {
            if (!self) {
                return rpc::Ready(
                    Unavailable(QStringLiteral("plugins are not loaded")));
            }
            QJsonArray loaded;
            for (const PluginManifest& plugin : self->Registry_.Plugins()) {
                loaded.append(QJsonObject{
                    {QStringLiteral("id"), plugin.Id},
                    {QStringLiteral("version"), plugin.Version},
                    {QStringLiteral("directory"), plugin.Directory},
                    {QStringLiteral("contributes"), plugin.Contributes},
                });
            }
            const QJsonObject list{
                {QStringLiteral("plugins"), loaded},
                {QStringLiteral("problems"),
                 QJsonArray::fromStringList(self->Registry_.Problems())},
            };
            // Refused rather than closing the connection.
            if (!Server::AnswerFitsAFrame(list)) {
                return rpc::Ready(std::unexpected(rpc::Error::FromCode(
                    rpc::EErrorCode::InternalError,
                    QStringLiteral("the plugin list does not fit a frame"))));
            }
            return rpc::Ready(list);
        });
    Q_ASSERT(plugins);
    Q_UNUSED(registered);
    Q_UNUSED(listing);
    Q_UNUSED(plugins);
}

PluginHost::~PluginHost() = default;

void PluginHost::Load(const QStringList& searchPath) {
    if (Loaded_) {
        return;
    }
    Loaded_ = true;
    for (const QString& directory : FindPluginDirectories(searchPath)) {
        auto manifest = LoadManifest(directory);
        if (!manifest) {
            Registry_.AddProblem(manifest.error());
            continue;
        }
        Registry_.Add(*std::move(manifest));
    }
    for (const PluginManifest& plugin : Registry_.Plugins()) {
        if (plugin.Backend.isEmpty()) {
            continue;
        }
        StartBackend(plugin);
        for (const PluginAgent& agent : plugin.Agents) {
            // An agent lost to an earlier plugin stays with that one; one
            // the plugin declares twice gets one bridge.
            if (Registry_.AgentOwner(agent.Id) == &plugin &&
                BridgeOf(agent.Id) == nullptr) {
                Bridges_.push_back(std::make_unique<AdapterBridge>(
                    agent.Id, agent.Capabilities, *BackendOf(plugin.Id)));
            }
        }
    }
}

Backend* PluginHost::BackendOf(const QString& pluginId) const {
    const auto it = Backends_.find(pluginId);
    return it == Backends_.end() ? nullptr : it->second.get();
}

AdapterBridge* PluginHost::BridgeOf(const QString& agentId) const {
    for (const auto& bridge : Bridges_) {
        if (bridge->Agent() == agentId) {
            return bridge.get();
        }
    }
    return nullptr;
}

void PluginHost::OnNotified(const QString& pluginId, const QString& method,
                            const QJsonValue& params) {
    if (method == QStringLiteral("session.stateChanged")) {
        OnStateChanged(pluginId, params.toObject());
    } else if (method == QStringLiteral("session.removed")) {
        OnRemoved(pluginId, params.toObject());
    }
}

bool PluginHost::ServesAgent(const QString& pluginId,
                             const QString& agent) const {
    const PluginManifest* owner = Registry_.AgentOwner(agent);
    return owner != nullptr && owner->Id == pluginId;
}

void PluginHost::OnStateChanged(const QString& pluginId,
                                const QJsonObject& params) {
    const QString agent = params.value(QStringLiteral("agent")).toString();
    if (!ServesAgent(pluginId, agent)) {
        qWarning(
            "roostd: the backend of %s told the state of agent \"%s\", "
            "which it does not serve",
            qUtf8Printable(pluginId), qUtf8Printable(agent));
        return;
    }
    const auto session =
        SessionFromJson(agent, params.value(QStringLiteral("session")));
    if (!session) {
        qWarning("roostd: the backend of %s told a bad session state: %s",
                 qUtf8Printable(pluginId), qUtf8Printable(session.error()));
        return;
    }
    const QString event = QStringLiteral("session.stateChanged");
    const QJsonObject payload = ToJson(*session);
    if (!Server::EventFitsAFrame(event, payload)) {
        qWarning(
            "roostd: the backend of %s told a session state too big for "
            "a frame",
            qUtf8Printable(pluginId));
        return;
    }
    Server_.Bus().Publish(event, payload);
}

void PluginHost::OnRemoved(const QString& pluginId, const QJsonObject& params) {
    const QString agent = params.value(QStringLiteral("agent")).toString();
    if (!ServesAgent(pluginId, agent)) {
        qWarning(
            "roostd: the backend of %s told the removal of a session of "
            "agent \"%s\", which it does not serve",
            qUtf8Printable(pluginId), qUtf8Printable(agent));
        return;
    }
    const QString id = params.value(QStringLiteral("session")).toString();
    if (id.isEmpty()) {
        qWarning(
            "roostd: the backend of %s told a removal without a session id",
            qUtf8Printable(pluginId));
        return;
    }
    const QString event = QStringLiteral("session.removed");
    const QJsonObject payload{
        {QStringLiteral("agent"), agent},
        {QStringLiteral("id"), id},
    };
    if (!Server::EventFitsAFrame(event, payload)) {
        qWarning("roostd: the backend of %s told a removal too big for a frame",
                 qUtf8Printable(pluginId));
        return;
    }
    Server_.Bus().Publish(event, payload);
}

void PluginHost::StartBackend(const PluginManifest& plugin) {
    auto backend = std::make_unique<Backend>(plugin.Id, plugin.Backend,
                                             plugin.Directory, Options_);
    const QString id = plugin.Id;
    connect(backend.get(), &Backend::Crashed, this,
            [this, id](const QString& reason, qint64 restartInMs) {
                qWarning(
                    "roostd: the backend of %s %s; restarting it in %lld "
                    "ms",
                    qUtf8Printable(id), qUtf8Printable(reason),
                    static_cast<long long>(restartInMs));
                Server_.Bus().Publish(
                    QStringLiteral("plugin.backendCrashed"),
                    QJsonObject{
                        {QStringLiteral("plugin"), id},
                        {QStringLiteral("reason"), reason},
                        {QStringLiteral("restartInMs"), restartInMs},
                    });
            });
    connect(backend.get(), &Backend::GaveUp, this,
            [this, id](const QString& reason) {
                qWarning("roostd: gave up on the backend of %s: %s",
                         qUtf8Printable(id), qUtf8Printable(reason));
                Server_.Bus().Publish(QStringLiteral("plugin.backendGaveUp"),
                                      QJsonObject{
                                          {QStringLiteral("plugin"), id},
                                          {QStringLiteral("reason"), reason},
                                      });
            });
    connect(backend.get(), &Backend::Notified, this,
            [this, id](const QString& method, const QJsonValue& params) {
                OnNotified(id, method, params);
            });
    backend->Start();
    Backends_.emplace(id, std::move(backend));
}

}  // namespace roost
