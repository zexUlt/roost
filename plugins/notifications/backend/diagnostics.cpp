#include "notifications/backend/diagnostics.hpp"

#include <cstdio>

namespace roost::notifications {

void Warn(const QString& message) {
    const QByteArray line = message.toUtf8();
    std::fprintf(stderr, "notifications: %s\n", line.constData());
    std::fflush(stderr);
}

}  // namespace roost::notifications
