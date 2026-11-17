#include "qml_api/js_callback.hpp"

#include <QJSValueList>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QPointer>
#include <QtLogging>

namespace roost {

QJsonValue toJson(QJSEngine& engine, const QJSValue& value) {
    if (value.isUndefined()) {
        return QJsonValue::Undefined;
    }
    // JSON.stringify itself, so that the result is what JS code expects:
    // undefined members and functions are dropped, a Date is ISO text in UTC.
    const QJSValue json =
        engine.globalObject().property(QStringLiteral("JSON"));
    const QJSValue text = json.property(QStringLiteral("stringify"))
                              .callWithInstance(json, {value});
    if (text.isError()) {
        qWarning("roost: a value is not JSON: %s", qPrintable(text.toString()));
        return QJsonValue::Undefined;
    }
    if (!text.isString()) {
        // A function or a symbol at the top level: stringify gives undefined.
        return QJsonValue::Undefined;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArrayLiteral("[") + text.toString().toUtf8() + ']', &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        document.array().isEmpty()) {
        return QJsonValue::Undefined;
    }
    return document.array().first();
}

QJSValue errorToJs(QJSEngine& engine, const rpc::Error& error) {
    QJSValue object = engine.newObject();
    object.setProperty(QStringLiteral("code"), error.Code);
    object.setProperty(QStringLiteral("message"), error.Message);
    if (!error.Data.isUndefined()) {
        object.setProperty(QStringLiteral("data"),
                           engine.toScriptValue(error.Data));
    }
    return object;
}

void answer(QJSEngine& engine, const QJSValue& callback, const QString& what,
            const rpc::Result& result) {
    if (!callback.isCallable()) {
        if (!result) {
            qWarning("roost: %s failed: %s (%d)", qPrintable(what),
                     qPrintable(result.error().Message), result.error().Code);
        }
        return;
    }
    QJSValueList arguments;
    if (result) {
        arguments << QJSValue(QJSValue::NullValue)
                  << engine.toScriptValue(*result);
    } else {
        arguments << errorToJs(engine, result.error())
                  << QJSValue(QJSValue::UndefinedValue);
    }
    const QJSValue returned = callback.call(arguments);
    if (returned.isError()) {
        qWarning("roost: the callback of %s threw: %s", qPrintable(what),
                 qPrintable(returned.toString()));
    }
}

void answerLater(QJSEngine& engine, QObject* context, const QJSValue& callback,
                 const QString& what, const rpc::Result& result) {
    // Through the event loop, even when the result is already there: a
    // callback never runs inside the JS call that asked for it.
    QMetaObject::invokeMethod(
        context,
        [engine = QPointer<QJSEngine>(&engine), callback, what, result] {
            if (engine) {
                answer(*engine, callback, what, result);
            }
        },
        Qt::QueuedConnection);
}

void callMethod(api::Api& api, QJSEngine& engine, QObject* context,
                const QString& method, const QJsonValue& params,
                const QJSValue& callback) {
    api.Dispatch()
        .Call(method, params, context)
        .then(context, [engine = QPointer<QJSEngine>(&engine), context,
                        callback, method](const rpc::Result& result) {
            if (engine) {
                answerLater(*engine, context, callback, method, result);
            }
        });
}

}  // namespace roost
