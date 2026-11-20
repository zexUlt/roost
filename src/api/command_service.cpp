#include "api/command_service.hpp"

#include <QJsonObject>

#include "api/thread_check.hpp"

namespace roost::api {

CommandService::CommandService(IoThread& io, QObject* parent)
    : QObject(parent), Io_(io) {}

bool CommandService::Register(const QString& command, const QObject* owner,
                              LocalHandler handler) {
    if (!CheckThread(*this, "CommandService::Register") || command.isEmpty() ||
        !handler || Locals_.contains(command)) {
        return false;
    }
    Locals_.insert(command, {.Handler = std::move(handler), .Owner = owner});
    if (owner != nullptr) {
        // The owner is gone before its handler could run again; the entry is
        // looked up by owner, in case the id was freed and taken by another.
        connect(owner, &QObject::destroyed, this, [this, command, owner] {
            if (const auto it = Locals_.constFind(command);
                it != Locals_.cend() && it->Owner == owner) {
                Locals_.erase(it);
            }
        });
    }
    return true;
}

bool CommandService::HasLocal(const QString& command) const {
    return Locals_.contains(command);
}

QFuture<rpc::Result> CommandService::Run(const QString& command,
                                         const QJsonValue& args) {
    if (!CheckThread(*this, "CommandService::Run")) {
        return rpc::Ready(std::unexpected(WrongThread()));
    }
    if (const auto it = Locals_.constFind(command); it != Locals_.cend()) {
        // A copy: the handler may take its own registration away.
        const LocalHandler handler = it->Handler;
        return rpc::Ready(handler(args));
    }
    return Io_.Request(QStringLiteral("commands.run"),
                       QJsonObject{{QStringLiteral("command"), command},
                                   {QStringLiteral("args"), args}});
}

}  // namespace roost::api
