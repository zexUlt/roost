#include "roostd/plugins.hpp"

#include <QJsonObject>
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
    Q_UNUSED(registered);
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
        if (!plugin.Backend.isEmpty()) {
            StartBackend(plugin);
        }
    }
}

Backend* PluginHost::BackendOf(const QString& pluginId) const {
    const auto it = Backends_.find(pluginId);
    return it == Backends_.end() ? nullptr : it->second.get();
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
    backend->Start();
    Backends_.emplace(id, std::move(backend));
}

}  // namespace roost
