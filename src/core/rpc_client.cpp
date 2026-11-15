#include "core/rpc_client.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QSignalBlocker>

#include <utility>

#include "core/protocol.hpp"
#include "core/version.hpp"

namespace roost {

namespace {

rpc::Result Lost(const QString& reason) {
    return std::unexpected(
        protocol::MakeError(protocol::EErrorCode::ConnectionLost, reason));
}

void Settle(QPromise<rpc::Result>& promise, rpc::Result result) {
    promise.addResult(std::move(result));
    promise.finish();
}

}  // namespace

RpcClient::RpcClient(QObject* parent)
    : QObject(parent), Socket_(new QLocalSocket(this)) {
    connect(Socket_, &QLocalSocket::connected, this, &RpcClient::OnConnected);
    connect(Socket_, &QLocalSocket::readyRead, this, &RpcClient::Read);
    connect(Socket_, &QLocalSocket::errorOccurred, this,
            [this] { Lose(Socket_->errorString()); });
    connect(Socket_, &QLocalSocket::disconnected, this,
            [this] { Lose(QStringLiteral("roostd closed the connection")); });
}

RpcClient::~RpcClient() {
    // Nobody is left to hear Disconnected, but the futures still finish.
    const QSignalBlocker blocker(this);
    Lose(QStringLiteral("the client is gone"));
}

QFuture<rpc::Result> RpcClient::Connect(const QString& socketPath) {
    if (State_ != EState::Idle) {
        return rpc::Ready(Lost(QStringLiteral("a client connects only once")));
    }
    State_ = EState::Connecting;
    HelloId_ = NextId_;
    QFuture<rpc::Result> hello =
        Request(QStringLiteral("hello"),
                QJsonObject{
                    {QStringLiteral("protocol"), protocol::VERSION},
                    {QStringLiteral("ghosttyVt"),
                     QString::fromStdString(GhosttyVtVersion())},
                });
    // May fail, or connect, before it returns; hello is already queued.
    Socket_->connectToServer(socketPath);
    return hello;
}

QFuture<rpc::Result> RpcClient::Request(const QString& method,
                                        const QJsonValue& params) {
    if (State_ == EState::Idle || State_ == EState::Lost) {
        return rpc::Ready(Lost(QStringLiteral("not connected to roostd")));
    }
    const qint64 id = NextId_++;
    QJsonObject request{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("method"), method},
    };
    if (!params.isUndefined()) {
        request.insert(QStringLiteral("params"), params);
    }
    const std::optional<QByteArray> frame = EncodeFrame(
        EFrameKind::Rpc, QJsonDocument(request).toJson(QJsonDocument::Compact));
    if (!frame) {
        return rpc::Ready(std::unexpected(rpc::Error::FromCode(
            rpc::EErrorCode::InvalidParams,
            QStringLiteral("the request does not fit a frame"))));
    }

    QPromise<rpc::Result> promise;
    promise.start();
    QFuture<rpc::Result> future = promise.future();
    Pending_.emplace(id, std::move(promise));
    if (State_ == EState::Connected) {
        Socket_->write(*frame);
    } else {
        Unsent_.append(*frame);
    }
    return future;
}

void RpcClient::Disconnect() {
    Lose(QStringLiteral("disconnected by the client"));
}

void RpcClient::OnConnected() {
    State_ = EState::Connected;
    Socket_->write(std::exchange(Unsent_, {}));
}

void RpcClient::Read() {
    Decoder_.Feed(Socket_->readAll());
    while (State_ == EState::Connected) {
        std::optional<Frame> frame = Decoder_.Next();
        if (!frame) {
            break;
        }
        // Session channels come with M2; nothing sends them yet.
        if (frame->Kind != EFrameKind::Rpc) {
            continue;
        }
        QJsonParseError error{};
        const QJsonDocument document =
            QJsonDocument::fromJson(frame->Body, &error);
        if (document.isObject()) {
            Dispatch(document.object());
        } else if (document.isArray()) {
            for (const QJsonValue& message : document.array()) {
                Dispatch(message);
            }
        } else {
            Lose(QStringLiteral("roostd sent a message that is not JSON-RPC"));
        }
    }
    if (Decoder_.Failed()) {
        Lose(QStringLiteral("roostd broke the framing: %1")
                 .arg(Decoder_.Error()));
    }
}

void RpcClient::Dispatch(const QJsonValue& value) {
    if (State_ != EState::Connected) {
        return;
    }
    const QJsonObject message = value.toObject();
    const QJsonValue method = message.value(QStringLiteral("method"));
    if (method.isString()) {
        // roostd sends notifications only; a request from it has no taker.
        if (!message.contains(QStringLiteral("id"))) {
            emit NotificationReceived(method.toString(),
                                      message.value(QStringLiteral("params")));
        }
        return;
    }
    const std::optional<qint64> id =
        rpc::ToInteger(message.value(QStringLiteral("id")));
    const auto pending = id ? Pending_.find(*id) : Pending_.end();
    if (pending == Pending_.end()) {
        Lose(QStringLiteral("roostd answered a request nobody made"));
        return;
    }
    QPromise<rpc::Result> promise = std::move(pending->second);
    Pending_.erase(pending);

    const QJsonValue error = message.value(QStringLiteral("error"));
    if (error.isObject()) {
        const QJsonObject object = error.toObject();
        const QString text = object.value(QStringLiteral("message")).toString();
        Settle(promise,
               std::unexpected(rpc::Error{
                   .Code = object.value(QStringLiteral("code")).toInt(),
                   .Message = text,
                   .Data = object.value(QStringLiteral("data")),
               }));
        // Nothing else would be served.
        if (*id == HelloId_) {
            Lose(QStringLiteral("roostd refused hello: %1").arg(text));
        }
        return;
    }
    const QJsonValue result = message.value(QStringLiteral("result"));
    Settle(promise, result.isUndefined() ? QJsonValue() : result);
}

void RpcClient::Lose(const QString& reason) {
    if (State_ == EState::Lost) {
        return;
    }
    const bool wasUp = State_ != EState::Idle;
    State_ = EState::Lost;
    Unsent_.clear();
    Socket_->abort();
    // Moved out first: a continuation run by a failed request may call
    // Request, which must not touch the map being walked.
    auto pending = std::exchange(Pending_, {});
    for (auto& [id, promise] : pending) {
        Settle(promise, Lost(reason));
    }
    if (wasUp) {
        emit Disconnected(reason);
    }
}

}  // namespace roost
