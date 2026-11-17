#include "qml_api/roost_singleton.hpp"

#include <QtLogging>

#include "qml_api/js_callback.hpp"

namespace roost {

RoostSingleton::RoostSingleton(QJSEngine& engine) : Engine_(engine) {}

RoostSingleton* RoostSingleton::create(QQmlEngine* /*qmlEngine*/,
                                       QJSEngine* jsEngine) {
    return new RoostSingleton(*jsEngine);
}

void RoostSingleton::setApi(api::Api& api) {
    if (Api_ != nullptr) {
        qWarning("roost: Roost.setApi called twice; ignored");
        return;
    }
    Api_ = &api;
    Commands_ = new CommandsAdapter(api, Engine_, this);
    Sessions_ = new SessionsModel(api, Engine_, this);
    Store_ = new StoreAdapter(api, Engine_, this);
    Bus_ = new BusAdapter(api, Engine_, this);
    Connected_ = api.IsConnected();
    connect(&api, &api::Api::Connected, this, [this] { setConnected(true); });
    connect(&api, &api::Api::Disconnected, this,
            [this] { setConnected(false); });
}

void RoostSingleton::call(const QString& method, const QJSValue& params,
                          const QJSValue& callback) {
    if (Api_ == nullptr) {
        answerLater(Engine_, this, callback, method,
                    std::unexpected(rpc::Error::FromCode(
                        rpc::EErrorCode::InternalError,
                        QStringLiteral("the API of the window is not set"))));
        return;
    }
    callMethod(*Api_, Engine_, this, method, toJson(Engine_, params), callback);
}

void RoostSingleton::setConnected(bool connected) {
    if (Connected_ != connected) {
        Connected_ = connected;
        emit connectedChanged();
    }
}

}  // namespace roost
