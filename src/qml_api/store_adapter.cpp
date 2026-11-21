#include "qml_api/store_adapter.hpp"

#include <QPointer>

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
    // An event can overtake the answer on its way to the callback (the answer
    // goes through the event loop once more): the callback gets the document
    // as it is when it is called, so that it never takes a newer one back.
    callMethod(Api_, Engine_, context, QStringLiteral("store.get"),
               QJsonObject{{QStringLiteral("plugin"), plugin}}, callback,
               [self = QPointer<StoreAdapter>(this),
                plugin](const rpc::Result& result) -> rpc::Result {
                   if (!result || self.isNull()) {
                       return result;
                   }
                   const auto document = self->Api_.Store().Cached(plugin);
                   if (!document) {
                       return result;
                   }
                   QJsonObject answer = result->toObject();
                   answer.insert(QStringLiteral("document"), *document);
                   return rpc::Result(answer);
               });
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

void StoreAdapter::patch(const QString& plugin, const QJSValue& patch,
                         const QJSValue& callback) {
    QObject* const context =
        Owners_.contextFor(Engine_, this, QStringLiteral("store.patch"));
    if (context == nullptr) {
        return;
    }
    QJsonObject params{{QStringLiteral("plugin"), plugin}};
    if (!patch.isUndefined()) {
        params.insert(QStringLiteral("patch"), toJson(Engine_, patch));
    }
    callMethod(Api_, Engine_, context, QStringLiteral("store.patch"), params,
               callback);
}

}  // namespace roost
