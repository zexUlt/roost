#include "api/plugin_service.hpp"

#include "api/thread_check.hpp"

namespace roost::api {

PluginService::PluginService(IoThread& io, QObject* parent)
    : QObject(parent), Io_(io) {}

void PluginService::Fetch() {
    if (!CheckThread(*this, "PluginService::Fetch") || Fetching_) {
        return;
    }
    Fetching_ = true;
    Fetch_ = Io_.Request(QStringLiteral("plugins.list"))
                 .then(this, [this](rpc::Result answer) {
                     Fetching_ = false;
                     if (answer) {
                         Cache_ = answer->toObject();
                         Loaded_ = true;
                         emit Changed();
                     }
                     return answer;
                 });
}

bool PluginService::IsLoaded() const {
    return CheckThread(*this, "PluginService::IsLoaded") && Loaded_;
}

const QJsonObject& PluginService::Plugins() const {
    static const QJsonObject NONE;
    return CheckThread(*this, "PluginService::Plugins") ? Cache_ : NONE;
}

QFuture<rpc::Result> PluginService::List() {
    if (!CheckThread(*this, "PluginService::List")) {
        return rpc::Ready(std::unexpected(WrongThread()));
    }
    if (Loaded_) {
        return rpc::Ready(QJsonValue(Cache_));
    }
    Fetch();
    return Fetch_;
}

}  // namespace roost::api
