#pragma once

#include <QJSEngine>
#include <QJSValue>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include "api/api.hpp"
#include "core/rpc.hpp"

namespace roost {

// A JS value as JSON.stringify has it: undefined members and functions are
// dropped, a Date is ISO text in UTC. A value that is undefined (or a function)
// itself is Undefined (an absent value), and so is one that is not JSON, with a
// warning.
QJsonValue toJson(QJSEngine& engine, const QJSValue& value);

// The error of a call as JS sees it: {code, message, data?}.
QJSValue errorToJs(QJSEngine& engine, const rpc::Error& error);

// Hands the answer to a JS callback as callback(error, result): error is null
// and result is the value on success; error is the object and result is
// undefined on failure. Call it in the GUI thread. `what` names the call in
// the log.
//
// A failure with no callback (absent or not callable) is logged as a warning,
// and so is a callback that throws: the exception does not go further.
void answer(QJSEngine& engine, const QJSValue& callback, const QString& what,
            const rpc::Result& result);

// Like `answer`, but always from the event loop, never inside the caller:
// `context` (a GUI-thread object) is where the event is posted, and when it is
// destroyed the answer is dropped.
void answerLater(QJSEngine& engine, QObject* context, const QJSValue& callback,
                 const QString& what, const rpc::Result& result);

// Calls `method` of the API by name and answers the callback with the result,
// in the GUI thread, never before the caller has returned. `context` must live
// in the GUI thread: when it is destroyed, or the engine is, the answer is
// dropped and the callback is not called.
void callMethod(api::Api& api, QJSEngine& engine, QObject* context,
                const QString& method, const QJsonValue& params,
                const QJSValue& callback);

}  // namespace roost
