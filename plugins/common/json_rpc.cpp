#include "common/json_rpc.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>

#include <optional>
#include <utility>

namespace roost::plugin::jsonrpc {

namespace {

const QString VERSION = QStringLiteral("2.0");

Result Fail(EErrorCode code, const QString& message) {
    return std::unexpected(Error::FromCode(code, message));
}

QJsonObject ErrorObject(const Error& error) {
    QJsonObject object{
        {QStringLiteral("code"), error.Code},
        {QStringLiteral("message"), error.Message},
    };
    if (!error.Data.isUndefined()) {
        object.insert(QStringLiteral("data"), error.Data);
    }
    return object;
}

QJsonValue Response(const QJsonValue& id, const Result& result) {
    QJsonObject object{{QStringLiteral("jsonrpc"), VERSION}};
    if (result) {
        // An undefined value would drop the key, and a response must carry
        // either "result" or "error".
        object.insert(
            QStringLiteral("result"),
            result->isUndefined() ? QJsonValue(QJsonValue::Null) : *result);
    } else {
        object.insert(QStringLiteral("error"), ErrorObject(result.error()));
    }
    object.insert(QStringLiteral("id"), id);
    return object;
}

QJsonValue ErrorResponse(const QJsonValue& id, EErrorCode code,
                         const QString& message) {
    return Response(id, Fail(code, message));
}

bool IsValidId(const QJsonValue& id) {
    return id.isString() || id.isDouble() || id.isNull();
}

QByteArray Serialize(const QJsonValue& value) {
    const QJsonDocument document = value.isArray()
                                       ? QJsonDocument(value.toArray())
                                       : QJsonDocument(value.toObject());
    return document.toJson(QJsonDocument::Compact);
}

// QJsonDocument only takes an object or an array at the top level. A bare
// scalar is still valid JSON, and the specification answers it with Invalid
// Request rather than Parse error, so a failed parse is retried as the single
// element of an array.
std::optional<QJsonValue> ParseJson(QByteArrayView message) {
    QJsonParseError error{};
    const QJsonDocument document =
        QJsonDocument::fromJson(message.toByteArray(), &error);
    if (error.error == QJsonParseError::NoError) {
        return document.isArray() ? QJsonValue(document.array())
                                  : QJsonValue(document.object());
    }
    QByteArray wrapped;
    wrapped.reserve(message.size() + 2);
    wrapped.append('[').append(message).append(']');
    const QJsonDocument retry = QJsonDocument::fromJson(wrapped, &error);
    if (error.error == QJsonParseError::NoError && retry.array().size() == 1) {
        return retry.array().first();
    }
    return std::nullopt;
}

// What a finished, not canceled, handler future amounts to.
Result Settle(QFuture<Result> done) {
    try {
        done.waitForFinished();  // rethrows what the handler failed with
    } catch (...) {
        return Fail(EErrorCode::InternalError,
                    QStringLiteral("Internal error: handler failed"));
    }
    if (done.resultCount() == 0) {
        return Fail(EErrorCode::InternalError,
                    QStringLiteral(
                        "Internal error: handler finished without a result"));
    }
    return done.result();
}

}  // namespace

Error Error::FromCode(EErrorCode code, QString message, QJsonValue data) {
    return Error{
        .Code = static_cast<int>(code),
        .Message = std::move(message),
        .Data = std::move(data),
    };
}

QFuture<Result> Ready(Result result) {
    return QtFuture::makeReadyValueFuture(std::move(result));
}

std::optional<qint64> ToInteger(const QJsonValue& value) {
    // toInteger gives its default for anything that is not an integer, so two
    // different defaults agree only on a real one.
    const qint64 integer = value.toInteger(0);
    if (integer != value.toInteger(1)) {
        return std::nullopt;
    }
    return integer;
}

bool Dispatcher::Register(const QString& method, Handler handler) {
    if (method.startsWith(QLatin1String("rpc.")) ||
        Handlers_.contains(method)) {
        return false;
    }
    Handlers_.insert(method, std::move(handler));
    return true;
}

QFuture<QByteArray> Dispatcher::Handle(QByteArrayView message) const {
    const std::optional<QJsonValue> parsed = ParseJson(message);
    if (!parsed) {
        return QtFuture::makeReadyValueFuture(
            Serialize(ErrorResponse(QJsonValue::Null, EErrorCode::ParseError,
                                    QStringLiteral("Parse error"))));
    }

    if (!parsed->isArray()) {
        return HandleOne(*parsed).then(
            QtFuture::Launch::Sync, [](const QJsonValue& response) {
                return response.isUndefined() ? QByteArray()
                                              : Serialize(response);
            });
    }

    const QJsonArray batch = parsed->toArray();
    if (batch.isEmpty()) {
        return QtFuture::makeReadyValueFuture(Serialize(
            ErrorResponse(QJsonValue::Null, EErrorCode::InvalidRequest,
                          QStringLiteral("Invalid Request: empty batch"))));
    }
    QList<QFuture<QJsonValue>> answers;
    answers.reserve(batch.size());
    for (const QJsonValue& element : batch) {
        answers.append(HandleOne(element));
    }
    return QtFuture::whenAll(answers.begin(), answers.end())
        .then(QtFuture::Launch::Sync,
              [](const QList<QFuture<QJsonValue>>& done) {
                  QJsonArray responses;
                  for (const QFuture<QJsonValue>& answer : done) {
                      const QJsonValue response = answer.result();
                      if (!response.isUndefined()) {
                          responses.append(response);
                      }
                  }
                  return responses.isEmpty() ? QByteArray()
                                             : Serialize(responses);
              });
}

// Yields the response object, or undefined for a notification.
QFuture<QJsonValue> Dispatcher::HandleOne(const QJsonValue& message) const {
    auto invalid = [](const QJsonValue& id, const QString& why) {
        return QtFuture::makeReadyValueFuture(
            ErrorResponse(id, EErrorCode::InvalidRequest,
                          QStringLiteral("Invalid Request: %1").arg(why)));
    };

    if (!message.isObject()) {
        return invalid(QJsonValue::Null, QStringLiteral("not an object"));
    }
    const QJsonObject request = message.toObject();
    const bool isNotification = !request.contains(QStringLiteral("id"));
    const QJsonValue id = request.value(QStringLiteral("id"));
    if (!isNotification && !IsValidId(id)) {
        return invalid(QJsonValue::Null,
                       QStringLiteral("id must be a string, number or null"));
    }
    // An invalid message is answered even without an id: the peer may have
    // meant a request, and silence would leave it waiting.
    const QJsonValue replyId = isNotification ? QJsonValue::Null : id;
    if (request.value(QStringLiteral("jsonrpc")) != VERSION) {
        return invalid(replyId, QStringLiteral("jsonrpc must be \"2.0\""));
    }
    const QJsonValue method = request.value(QStringLiteral("method"));
    if (!method.isString()) {
        return invalid(replyId, QStringLiteral("method must be a string"));
    }
    const QJsonValue params = request.value(QStringLiteral("params"));
    if (!params.isUndefined() && !params.isObject() && !params.isArray()) {
        return invalid(replyId,
                       QStringLiteral("params must be an object or an array"));
    }

    QFuture<Result> answer;
    const auto handler = Handlers_.constFind(method.toString());
    if (handler == Handlers_.cend()) {
        answer = Ready(Fail(
            EErrorCode::MethodNotFound,
            QStringLiteral("Method not found: %1").arg(method.toString())));
    } else {
        try {
            answer = (*handler)(Call{
                .Method = method.toString(),
                .Params = params,
            });
        } catch (...) {
            answer =
                Ready(Fail(EErrorCode::InternalError,
                           QStringLiteral("Internal error: handler threw")));
        }
    }

    // then() with the whole future settles a failure and a missing result;
    // onFailed would instead pass a healthy future on by copying its result,
    // which crashes on a promise finished without one. A canceled future
    // skips then(), so onCanceled comes after it, where the value it passes
    // on always exists.
    auto reply = [isNotification, id](const Result& result) {
        // A notification is never answered, not even with an error.
        return isNotification ? QJsonValue(QJsonValue::Undefined)
                              : Response(id, result);
    };
    return answer
        .then(QtFuture::Launch::Sync,
              [reply](QFuture<Result> done) { return reply(Settle(done)); })
        .onCanceled([reply] {
            return reply(Fail(
                EErrorCode::InternalError,
                QStringLiteral("Internal error: handler dropped the request")));
        });
}

}  // namespace roost::plugin::jsonrpc
