#include "helpers/scoped_env.hpp"

#include <QtGlobal>

namespace roost::testing {

ScopedEnv::ScopedEnv(const char* name, std::optional<QByteArray> value)
    : Name_(name),
      Old_(qEnvironmentVariableIsSet(name) ? std::optional(qgetenv(name))
                                           : std::nullopt) {
    Set(value);
}

ScopedEnv::~ScopedEnv() { Set(Old_); }

void ScopedEnv::Set(const std::optional<QByteArray>& value) {
    if (value) {
        qputenv(Name_, *value);
    } else {
        qunsetenv(Name_);
    }
}

}  // namespace roost::testing
