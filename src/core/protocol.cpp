#include "core/protocol.hpp"

#include <QDir>

#include <unistd.h>

#include <utility>

namespace roost::protocol {

rpc::Error MakeError(EErrorCode code, QString message, QJsonValue data) {
    return rpc::Error{
        .Code = static_cast<int>(code),
        .Message = std::move(message),
        .Data = std::move(data),
    };
}

QString SocketPath() {
    const QString explicitPath = qEnvironmentVariable("ROOST_SOCKET");
    if (!explicitPath.isEmpty()) {
        return QDir::cleanPath(QDir(explicitPath).absolutePath());
    }
    // The XDG spec says a relative value is invalid and must be ignored.
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (QDir::isAbsolutePath(runtime)) {
        return QDir::cleanPath(runtime + QStringLiteral("/roost/roostd.sock"));
    }
    // /tmp is shared on Linux: the uid keeps users out of each other's way.
    return QDir::cleanPath(
        QDir::tempPath() +
        QStringLiteral("/roost-%1/roostd.sock").arg(::geteuid()));
}

}  // namespace roost::protocol
