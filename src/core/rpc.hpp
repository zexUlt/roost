#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QFuture>
#include <QHash>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <expected>
#include <functional>
#include <optional>

// JSON-RPC 2.0 (https://www.jsonrpc.org/specification), the server side: a
// method table and the dispatch of requests, notifications and batches. No
// I/O: a message body comes in, a response body comes out.
namespace roost::rpc {

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

// One request as a handler sees it. Peer identifies the connection the request
// came in on, so a handler can tie state such as subscriptions to it; the
// dispatcher never dereferences it.
struct Call {
    QString Method;
    QJsonValue Params = QJsonValue::Undefined;  // an object, an array or absent
    QObject* Peer = nullptr;
};

// A handler answers through a future, so it can wait on a backend process. One
// that knows the answer at once returns Ready(...). The Call is gone once the
// handler returns: an asynchronous handler copies what it needs. The response
// is built on the thread that completes the future.
using Handler = std::function<QFuture<Result>(const Call&)>;

QFuture<Result> Ready(Result result);

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
    //
    // Numeric ids pass through a double: integers beyond 2^53 come back
    // rounded. roost's own clients send small integers.
    QFuture<QByteArray> Handle(QByteArrayView message, QObject* peer) const;

private:
    QFuture<QJsonValue> HandleOne(const QJsonValue& message,
                                  QObject* peer) const;

    QHash<QString, Handler> Handlers_;
};

}  // namespace roost::rpc
