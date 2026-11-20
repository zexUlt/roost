#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QFuture>
#include <QHash>
#include <QJsonValue>
#include <QString>

#include <expected>
#include <functional>
#include <optional>

// JSON-RPC 2.0 (https://www.jsonrpc.org/specification), the server side, as the
// adapter backends need it: a method table and the dispatch of requests,
// notifications and batches. No I/O and no dependency on roost internals.
namespace roost::plugin::jsonrpc {

// The error codes the specification reserves.
enum class EErrorCode : int {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
};

struct Error {
    int Code = static_cast<int>(EErrorCode::InternalError);
    QString Message;
    QJsonValue Data = QJsonValue::Undefined;  // omitted from the response

    static Error FromCode(EErrorCode code, QString message,
                          QJsonValue data = QJsonValue::Undefined);
};

using Result = std::expected<QJsonValue, Error>;

// One request as a handler sees it.
struct Call {
    QString Method;
    QJsonValue Params = QJsonValue::Undefined;  // an object, an array or absent
};

// A handler answers through a future, so it can wait on a process. One that
// knows the answer at once returns Ready(...). The Call is gone once the
// handler returns: an asynchronous handler copies what it needs.
using Handler = std::function<QFuture<Result>(const Call&)>;

QFuture<Result> Ready(Result result);

// An integer, as JSON numbers carry one: 3 and 3.0, not 3.5, "3" or 2^64.
std::optional<qint64> ToInteger(const QJsonValue& value);

class Dispatcher {
public:
    // False if the method is already taken, or the name is reserved by the
    // specification (starts with "rpc.").
    [[nodiscard]] bool Register(const QString& method, Handler handler);

    // Handles one message: a request, a notification or a batch of them. The
    // future yields the response body, or an empty array when the message
    // needs none (notifications only). A handler that throws, fails its
    // future, drops its promise or finishes it without a result answers with
    // InternalError.
    QFuture<QByteArray> Handle(QByteArrayView message) const;

private:
    QFuture<QJsonValue> HandleOne(const QJsonValue& message) const;

    QHash<QString, Handler> Handlers_;
};

}  // namespace roost::plugin::jsonrpc
