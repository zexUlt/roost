#include "core/ghostty_result.hpp"

#include <QLatin1StringView>

namespace roost {

namespace {

QString Describe(GhosttyResult result) {
    switch (result) {
        case GHOSTTY_OUT_OF_MEMORY:
            return QStringLiteral("out of memory");
        case GHOSTTY_INVALID_VALUE:
            return QStringLiteral("invalid value");
        default:
            return QStringLiteral("error %1").arg(static_cast<int>(result));
    }
}

}  // namespace

QString Failed(const char* what, GhosttyResult result) {
    return QStringLiteral("%1: %2").arg(QLatin1StringView(what),
                                        Describe(result));
}

}  // namespace roost
