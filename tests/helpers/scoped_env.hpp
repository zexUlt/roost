#pragma once

#include <QByteArray>

#include <optional>

namespace roost::testing {

// Sets or unsets an environment variable for one scope.
class ScopedEnv {
public:
    ScopedEnv(const char* name, std::optional<QByteArray> value);
    ~ScopedEnv();
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void Set(const std::optional<QByteArray>& value);

    const char* Name_;
    std::optional<QByteArray> Old_;
};

}  // namespace roost::testing
