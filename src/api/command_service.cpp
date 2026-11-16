#include "api/command_service.hpp"

#include <QJsonObject>

#include "api/thread_check.hpp"

namespace roost::api {

CommandService::CommandService(IoThread& io, QObject* parent)
    : QObject(parent), Io_(io) {}

QFuture<rpc::Result> CommandService::Run(const QString& command,
                                         const QJsonValue& args) {
    if (!CheckThread(*this, "CommandService::Run")) {
        return rpc::Ready(std::unexpected(WrongThread()));
    }
    return Io_.Request(QStringLiteral("commands.run"),
                       QJsonObject{{QStringLiteral("command"), command},
                                   {QStringLiteral("args"), args}});
}

}  // namespace roost::api
