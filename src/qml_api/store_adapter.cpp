#include "qml_api/store_adapter.hpp"

#include "qml_api/js_callback.hpp"

namespace roost {

StoreAdapter::StoreAdapter(api::Api& api, QJSEngine& engine,
                           const CallOwners& owners, QObject* parent)
    : QObject(parent), Api_(api), Engine_(engine), Owners_(owners) {
    connect(&Api_.Store(), &api::StoreService::Changed, this,
            &StoreAdapter::changed);
}

void StoreAdapter::get(const QString& plugin, const QJSValue& callback) {
    QObject* const context =
        Owners_.contextFor(Engine_, this, QStringLiteral("store.get"));
    if (context == nullptr) {
        return;
    }
    callMethod(Api_, Engine_, context, QStringLiteral("store.get"),
               QJsonObject{{QStringLiteral("plugin"), plugin}}, callback);
}

void StoreAdapter::set(const QString& plugin, const QJSValue& document,
                       const QJSValue& callback) {
    QObject* const context =
        Owners_.contextFor(Engine_, this, QStringLiteral("store.set"));
    if (context == nullptr) {
        return;
    }
    QJsonObject params{{QStringLiteral("plugin"), plugin}};
    if (!document.isUndefined()) {
        params.insert(QStringLiteral("document"), toJson(Engine_, document));
    }
    callMethod(Api_, Engine_, context, QStringLiteral("store.set"), params,
               callback);
}

}  // namespace roost
