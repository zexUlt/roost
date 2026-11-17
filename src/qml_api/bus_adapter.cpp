#include "qml_api/bus_adapter.hpp"

#include <QJSValueList>
#include <QJsonObject>
#include <QPointer>
#include <QtLogging>

#include "qml_api/js_callback.hpp"

namespace roost {

BusAdapter::BusAdapter(api::Api& api, QJSEngine& engine, QObject* parent)
    : QObject(parent), Api_(api), Engine_(engine) {}

qulonglong BusAdapter::subscribe(const QString& pattern, QObject* owner,
                                 const QJSValue& callback) {
    if (owner == nullptr) {
        Engine_.throwError(QJSValue::TypeError,
                           QStringLiteral("Roost.bus.subscribe: the owner "
                                          "must be an object, not null"));
        return 0;
    }
    if (!callback.isCallable()) {
        Engine_.throwError(QJSValue::TypeError,
                           QStringLiteral("Roost.bus.subscribe: the callback "
                                          "must be a function"));
        return 0;
    }
    const auto id = Api_.Dispatch().Subscribe(
        pattern, owner,
        [engine = QPointer<QJSEngine>(&Engine_), callback](
            const QString& name, const QJsonValue& payload) {
            if (!engine) {
                return;
            }
            const QJSValue returned =
                callback.call({QJSValue(name), engine->toScriptValue(payload)});
            if (returned.isError()) {
                qWarning("roost: a bus callback for %s threw: %s",
                         qPrintable(name), qPrintable(returned.toString()));
            }
        });
    if (!id) {
        Engine_.throwError(QJSValue::TypeError, id.error().Message);
        return 0;
    }
    return *id;
}

void BusAdapter::unsubscribe(qulonglong id) { Api_.Dispatch().Unsubscribe(id); }

void BusAdapter::publish(const QString& name, const QJSValue& payload,
                         const QJSValue& callback) {
    QJsonObject params{{QStringLiteral("name"), name}};
    if (!payload.isUndefined()) {
        params.insert(QStringLiteral("payload"), toJson(Engine_, payload));
    }
    callMethod(Api_, Engine_, this, QStringLiteral("bus.publish"), params,
               callback);
}

}  // namespace roost
