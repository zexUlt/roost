#include "api/dispatcher.hpp"

#include <QJsonArray>
#include <QJsonObject>

#include <expected>
#include <utility>

#include "api/command_service.hpp"
#include "api/keymap_service.hpp"
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

// Whether the plugin declares the command in its `contributes.commands`: the
// list that plugins.list gives, which is what ownership of a command is. The
// prefix of an id says nothing: a plugin id may hold dots (`a` and `a.b`), and
// the builtin shell declares `terminal.copy` and `layout.*`.
bool Declares(const QJsonObject& list, const QString& plugin,
              const QString& command) {
    for (const QJsonValue& value :
         list.value(QStringLiteral("plugins")).toArray()) {
        const QJsonObject declared = value.toObject();
        if (declared.value(QStringLiteral("id")).toString() != plugin) {
            continue;
        }
        const QJsonArray commands =
            declared.value(QStringLiteral("contributes"))
                .toObject()
                .value(QStringLiteral("commands"))
                .toArray();
        for (const QJsonValue& item : commands) {
            if (item.toObject().value(QStringLiteral("id")).toString() ==
                command) {
                return true;
            }
        }
    }
    return false;
}

// The command a keymap call names, or the refusal: a plugin manages the keys
// of the commands it declares itself, in its own `contributes.commands`, and
// no others; it may give such a command any chord, a free one or one that
// another command holds (the window warns the user of that, a plugin does
// not). An empty `plugin` is the window, which sets any command.
std::expected<QString, rpc::Error> KeymapCommand(const rpc::Call& call,
                                                 const QString& method,
                                                 const PluginService& plugins) {
    const QJsonValue command =
        call.Params.toObject().value(QStringLiteral("command"));
    if (!command.isString()) {
        return std::unexpected(rpc::Error::FromCode(
            rpc::EErrorCode::InvalidParams,
            QStringLiteral("%1 takes {command, args?, "
                           "when?%2}: command is a "
                           "command id")
                .arg(method, method.endsWith(u"setCommand")
                                 ? QStringLiteral(", entries")
                                 : QString())));
    }
    if (call.Plugin.isEmpty()) {
        return command.toString();
    }
    if (!plugins.IsLoaded()) {
        return std::unexpected(rpc::Error::FromCode(
            rpc::EErrorCode::InvalidParams,
            QStringLiteral("%1 of plugin %2 is refused for %3: the plugins "
                           "are not loaded yet, so what the plugin declares "
                           "is not known")
                .arg(method, call.Plugin, command.toString())));
    }
    if (!Declares(plugins.Plugins(), call.Plugin, command.toString())) {
        return std::unexpected(rpc::Error::FromCode(
            rpc::EErrorCode::InvalidParams,
            QStringLiteral("%1 of plugin %2 is refused for %3: a plugin sets "
                           "the keys of the commands it declares in its own "
                           "contributes.commands")
                .arg(method, call.Plugin, command.toString())));
    }
    return command.toString();
}

// The `when` of a keymap call: a string, empty if there is none.
std::expected<QString, rpc::Error> KeymapWhen(const rpc::Call& call,
                                              const QString& method) {
    const QJsonValue when =
        call.Params.toObject().value(QStringLiteral("when"));
    if (when.isUndefined() || when.isNull()) {
        return QString();
    }
    if (!when.isString()) {
        return std::unexpected(rpc::Error::FromCode(
            rpc::EErrorCode::InvalidParams,
            QStringLiteral("%1: when is the type of a panel, a string")
                .arg(method)));
    }
    return when.toString();
}

}  // namespace

Dispatcher::Dispatcher(BusService& bus, CommandService& commands,
                       KeymapService& keymap, PluginService& plugins,
                       SessionService& sessions, StoreService& store,
                       QObject* parent)
    : QObject(parent), Bus_(bus) {
    RegisterBuiltins(commands, keymap, plugins, sessions, store);
}

void Dispatcher::RegisterBuiltins(CommandService& commands,
                                  KeymapService& keymap, PluginService& plugins,
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
                    "store.get takes {plugin}: the id of a loaded "
                    "plugin, \"layout\" or \"settings\"")));
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
                                   "id of a loaded plugin, \"layout\" or "
                                   "\"settings\", and a JSON object")));
            }
            return store.Set(plugin.toString(),
                             params.value(QStringLiteral("document")));
        });
    const bool patching = Register(
        QStringLiteral("store.patch"), [&store](const rpc::Call& call) {
            const QJsonObject params = call.Params.toObject();
            const QJsonValue plugin = params.value(QStringLiteral("plugin"));
            if (!plugin.isString()) {
                return rpc::Ready(InvalidParams(
                    QStringLiteral("store.patch takes {plugin, patch}: the "
                                   "id of a loaded plugin, \"layout\" or "
                                   "\"settings\", and a JSON object "
                                   "(RFC 7386 merge patch)")));
            }
            return store.Patch(plugin.toString(),
                               params.value(QStringLiteral("patch")));
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
    const bool keymapGetting = Register(
        QStringLiteral("keymap.get"), [&keymap](const rpc::Call& call) {
            // The table is the window's own platform's; the platform is not
            // a choice here, as it is for roostd.
            const QJsonValue platform =
                call.Params.toObject().value(QStringLiteral("platform"));
            if (platform.isUndefined() ||
                platform == KeymapService::PlatformName()) {
                return keymap.Get();
            }
            if (platform == KeymapService::OtherPlatformName()) {
                return keymap.GetOtherPlatform();
            }
            return rpc::Ready(InvalidParams(QStringLiteral(
                "keymap.get takes {platform?}: \"mac\" or "
                "\"linux\", the window's own if there is none")));
        });
    const bool keymapSetting = Register(
        QStringLiteral("keymap.setCommand"),
        [&keymap, &plugins](const rpc::Call& call) {
            const QString method = QStringLiteral("keymap.setCommand");
            const auto command = KeymapCommand(call, method, plugins);
            const auto when = KeymapWhen(call, method);
            const QJsonValue entries =
                call.Params.toObject().value(QStringLiteral("entries"));
            if (!command) {
                return rpc::Ready(std::unexpected(command.error()));
            }
            if (!when) {
                return rpc::Ready(std::unexpected(when.error()));
            }
            if (!entries.isArray()) {
                return rpc::Ready(InvalidParams(
                    QStringLiteral("keymap.setCommand takes {command, args?, "
                                   "when?, entries}: entries is an array of "
                                   "all the keys of the command, each {key?, "
                                   "mac?}; an empty array removes them all")));
            }
            return keymap.SetCommand(
                *command, entries.toArray(), *when,
                call.Params.toObject().value(QStringLiteral("args")));
        });
    const bool keymapResetting = Register(
        QStringLiteral("keymap.resetCommand"),
        [&keymap, &plugins](const rpc::Call& call) {
            const QString method = QStringLiteral("keymap.resetCommand");
            const auto command = KeymapCommand(call, method, plugins);
            const auto when = KeymapWhen(call, method);
            if (!command) {
                return rpc::Ready(std::unexpected(command.error()));
            }
            if (!when) {
                return rpc::Ready(std::unexpected(when.error()));
            }
            return keymap.ResetCommand(
                *command, *when,
                call.Params.toObject().value(QStringLiteral("args")));
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
    Q_ASSERT(listingSessions && getting && setting && patching && listing &&
             running && keymapGetting && keymapSetting && keymapResetting &&
             publishing);
    Q_UNUSED(listingSessions)
    Q_UNUSED(getting)
    Q_UNUSED(setting)
    Q_UNUSED(patching)
    Q_UNUSED(listing)
    Q_UNUSED(running)
    Q_UNUSED(keymapGetting)
    Q_UNUSED(keymapSetting)
    Q_UNUSED(keymapResetting)
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

bool Dispatcher::JudgesPlugin(const QString& method) {
    return method == QStringLiteral("keymap.setCommand") ||
           method == QStringLiteral("keymap.resetCommand");
}

QFuture<rpc::Result> Dispatcher::Call(const QString& method,
                                      const QJsonValue& params,
                                      QObject* context, const QString& plugin) {
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
            .Params = params.isNull() ? QJsonValue::Undefined : params,
            .Plugin = plugin})
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
