#include "api/dispatcher.hpp"

#include <QJsonObject>

#include <utility>

#include "api/command_service.hpp"
#include "api/plugin_service.hpp"
#include "api/session_service.hpp"
#include "api/store_service.hpp"
#include "api/thread_check.hpp"

namespace roost::api {

namespace {

rpc::Result InvalidParams(const QString& message) {
    return std::unexpected(
        rpc::Error::FromCode(rpc::EErrorCode::InvalidParams, message));
}

}  // namespace

Dispatcher::Dispatcher(BusService& bus, CommandService& commands,
                       PluginService& plugins, SessionService& sessions,
                       StoreService& store, QObject* parent)
    : QObject(parent), Bus_(bus) {
    RegisterBuiltins(commands, plugins, sessions, store);
}

void Dispatcher::RegisterBuiltins(CommandService& commands,
                                  PluginService& plugins,
                                  SessionService& sessions,
                                  StoreService& store) {
    const bool listingSessions = Register(
        QStringLiteral("sessions.list"),
        [&sessions](const rpc::Call& /*call*/) { return sessions.Refresh(); });
    const bool getting =
        Register(QStringLiteral("store.get"), [&store](const rpc::Call& call) {
            const QJsonValue plugin =
                call.Params.toObject().value(QStringLiteral("plugin"));
            if (!plugin.isString()) {
                return rpc::Ready(InvalidParams(QStringLiteral(
                    "store.get takes {plugin}: the id of a loaded plugin")));
            }
            return store.Get(plugin.toString());
        });
    const bool setting =
        Register(QStringLiteral("store.set"), [&store](const rpc::Call& call) {
            const QJsonObject params = call.Params.toObject();
            const QJsonValue plugin = params.value(QStringLiteral("plugin"));
            if (!plugin.isString()) {
                return rpc::Ready(InvalidParams(
                    QStringLiteral("store.set takes {plugin, document}: the "
                                   "id of a loaded plugin and a JSON object")));
            }
            return store.Set(plugin.toString(),
                             params.value(QStringLiteral("document")));
        });
    const bool listing = Register(
        QStringLiteral("plugins.list"),
        [&plugins](const rpc::Call& /*call*/) { return plugins.List(); });
    const bool running = Register(
        QStringLiteral("commands.run"), [&commands](const rpc::Call& call) {
            const QJsonObject params = call.Params.toObject();
            const QJsonValue command = params.value(QStringLiteral("command"));
            if (!command.isString()) {
                return rpc::Ready(InvalidParams(QStringLiteral(
                    "commands.run takes {command, args?}: a command id and "
                    "any JSON value")));
            }
            return commands.Run(command.toString(),
                                params.value(QStringLiteral("args")));
        });
    const bool publishing =
        Register(QStringLiteral("bus.publish"), [this](const rpc::Call& call) {
            const QJsonObject params = call.Params.toObject();
            const QJsonValue name = params.value(QStringLiteral("name"));
            if (!name.isString()) {
                return rpc::Ready(InvalidParams(QStringLiteral(
                    "bus.publish takes {name, payload}: a dot-separated "
                    "event name and any JSON value")));
            }
            return Bus_.Publish(name.toString(),
                                params.value(QStringLiteral("payload")));
        });
    Q_ASSERT(listingSessions && getting && setting && listing && running &&
             publishing);
    Q_UNUSED(listingSessions)
    Q_UNUSED(getting)
    Q_UNUSED(setting)
    Q_UNUSED(listing)
    Q_UNUSED(running)
    Q_UNUSED(publishing)
}

bool Dispatcher::Register(const QString& method, rpc::Handler handler) {
    if (!CheckThread(*this, "Dispatcher::Register")) {
        return false;
    }
    return !method.startsWith(QStringLiteral("rpc.")) &&
           Handlers_.try_emplace(method, std::move(handler)).second;
}

QStringList Dispatcher::Methods() const {
    if (!CheckThread(*this, "Dispatcher::Methods")) {
        return {};
    }
    QStringList names;
    for (const auto& [name, handler] : Handlers_) {
        names.append(name);
    }
    return names;
}

QFuture<rpc::Result> Dispatcher::Call(const QString& method,
                                      const QJsonValue& params,
                                      QObject* context) {
    if (!CheckThread(*this, "Dispatcher::Call")) {
        return rpc::Ready(std::unexpected(WrongThread()));
    }
    const auto handler = Handlers_.find(method);
    if (handler == Handlers_.end()) {
        return rpc::Ready(std::unexpected(rpc::Error::FromCode(
            rpc::EErrorCode::MethodNotFound,
            QStringLiteral("method %1 not found").arg(method))));
    }
    // JSON-RPC params are structured or absent; null, what a caller without
    // params has, is absent. The handler's future may finish in the IO
    // thread: hop to the context.
    return handler
        ->second(rpc::Call{
            .Method = method,
            .Params = params.isNull() ? QJsonValue::Undefined : params})
        .then(context != nullptr ? context : this,
              [](rpc::Result answer) { return answer; });
}

std::expected<BusService::LocalId, rpc::Error> Dispatcher::Subscribe(
    const QString& pattern, const QObject* context,
    BusService::Listener listener) {
    if (!CheckThread(*this, "Dispatcher::Subscribe")) {
        return std::unexpected(WrongThread());
    }
    return Bus_.Subscribe(pattern, context, std::move(listener));
}

void Dispatcher::Unsubscribe(BusService::LocalId id) {
    if (CheckThread(*this, "Dispatcher::Unsubscribe")) {
        Bus_.Unsubscribe(id);
    }
}

}  // namespace roost::api
