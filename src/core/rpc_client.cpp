#include "core/rpc_client.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QSignalBlocker>

#include <sys/socket.h>
#include <sys/types.h>
#ifdef __APPLE__
#include <sys/un.h>
#endif

#include <algorithm>
#include <optional>
#include <utility>

#include "core/protocol.hpp"
#include "core/version.hpp"

namespace roost {

namespace {

rpc::Result Lost(const QString& reason,
                 const QJsonValue& data = QJsonValue::Undefined) {
    return std::unexpected(protocol::MakeError(
        protocol::EErrorCode::ConnectionLost, reason, data));
}

void Settle(QPromise<rpc::Result>& promise, rpc::Result result) {
    promise.addResult(std::move(result));
    promise.finish();
}

// The pid of the other end of a connected unix socket; 0 if unknown.
qint64 PeerPidOf(qintptr descriptor) {
    if (descriptor < 0) {
        return 0;
    }
#ifdef __APPLE__
    pid_t pid = 0;
    socklen_t length = sizeof(pid);
    if (::getsockopt(static_cast<int>(descriptor), SOL_LOCAL, LOCAL_PEERPID,
                     &pid, &length) != 0) {
        return 0;
    }
    return pid;
#elif defined(__linux__)
    // struct ucred needs _GNU_SOURCE, which clang defines in C++ on Linux.
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(static_cast<int>(descriptor), SOL_SOCKET, SO_PEERCRED,
                     &credentials, &length) != 0) {
        return 0;
    }
    return credentials.pid;
#else
    return 0;
#endif
}

}  // namespace

RpcClient::RpcClient(QObject* parent)
    : QObject(parent), Socket_(new QLocalSocket(this)) {
    connect(Socket_, &QLocalSocket::connected, this, &RpcClient::OnConnected);
    connect(Socket_, &QLocalSocket::readyRead, this, &RpcClient::Read);
    connect(Socket_, &QLocalSocket::errorOccurred, this,
            [this](QLocalSocket::LocalSocketError error) {
                // No socket file, or a file nobody listens on.
                const bool noListener =
                    error == QLocalSocket::ServerNotFoundError ||
                    error == QLocalSocket::ConnectionRefusedError;
                QJsonValue data;
                if (noListener) {
                    data = QJsonObject{
                        {QLatin1String(protocol::NO_LISTENER_KEY), true}};
                }
                Lose(Socket_->errorString(), data);
            });
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

void RpcClient::SendInput(std::uint32_t channel, QByteArrayView bytes) {
    if (State_ != EState::Connected) {
        return;
    }
    do {
        const QByteArrayView piece =
            bytes.first(std::min<qsizetype>(bytes.size(), MAX_CHANNEL_BYTES));
        Socket_->write(
            EncodeChannelFrame(EFrameKind::Input, channel, piece).value());
        bytes = bytes.sliced(piece.size());
    } while (!bytes.isEmpty());
}

void RpcClient::Disconnect() {
    Lose(QStringLiteral("disconnected by the client"));
}

void RpcClient::OnConnected() {
    State_ = EState::Connected;
    PeerPid_ = PeerPidOf(Socket_->socketDescriptor());
    Socket_->write(std::exchange(Unsent_, {}));
}

void RpcClient::Read() {
    Decoder_.Feed(Socket_->readAll());
    while (State_ == EState::Connected) {
        std::optional<Frame> frame = Decoder_.Next();
        if (!frame) {
            break;
        }
        if (frame->Kind != EFrameKind::Rpc) {
            const std::optional<ChannelBody> body =
                DecodeChannelBody(frame->Body);
            if (frame->Kind == EFrameKind::Input) {
                Lose(QStringLiteral("roostd sent an input frame"));
            } else if (!body) {
                Lose(
                    QStringLiteral("roostd sent a channel frame without a "
                                   "channel"));
            } else if (frame->Kind == EFrameKind::Output) {
                emit OutputReceived(body->Channel, body->Bytes);
            } else {
                emit SnapshotReceived(body->Channel, body->Bytes);
            }
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
        QJsonValue data = object.value(QStringLiteral("data"));
        if (*id == HelloId_ && PeerPid_ > 0 &&
            (data.isObject() || data.isUndefined() || data.isNull())) {
            // Who refused, as the system knows it: the window may stop
            // exactly this process.
            QJsonObject withPeer = data.toObject();
            withPeer.insert(QLatin1String(protocol::PEER_PID_KEY), PeerPid_);
            data = withPeer;
        }
        Settle(promise,
               std::unexpected(rpc::Error{
                   .Code = object.value(QStringLiteral("code")).toInt(),
                   .Message = text,
                   .Data = data,
               }));
        // Nothing else would be served.
        if (*id == HelloId_) {
            Lose(QStringLiteral("roostd refused hello: %1").arg(text));
        }
        return;
    }
    const QJsonValue result = message.value(QStringLiteral("result"));
    if (*id == HelloId_) {
        // The protocol and the library match, but a roostd left running from
        // before an update is still not the build of this window (its plugin
        // backends may sit in a deleted bundle): refuse it as roostd refuses
        // another protocol, so that the window can offer the restart.
        const QJsonValue theirs =
            result.toObject().value(QStringLiteral("roostd"));
        const QString ours = QString::fromStdString(VersionString());
        if (!theirs.isString() || theirs.toString() != ours) {
            // Null, not absent, when it is missing: the window tells this
            // refusal from the one for the protocol by the key.
            QJsonObject data{
                {QStringLiteral("roostd"),
                 theirs.isString() ? theirs : QJsonValue(QJsonValue::Null)}};
            if (PeerPid_ > 0) {
                data.insert(QLatin1String(protocol::PEER_PID_KEY), PeerPid_);
            }
            const QString text =
                theirs.isString()
                    ? QStringLiteral(
                          "roostd %1 is not the version of this window %2; "
                          "restart roostd")
                          .arg(theirs.toString(), ours)
                    : QStringLiteral(
                          "roostd did not tell its version, this window is "
                          "%1; restart roostd")
                          .arg(ours);
            Settle(promise,
                   std::unexpected(protocol::MakeError(
                       protocol::EErrorCode::VersionMismatch, text, data)));
            Lose(QStringLiteral("roostd refused hello: %1").arg(text));
            return;
        }
    }
    Settle(promise, result.isUndefined() ? QJsonValue() : result);
}

void RpcClient::Lose(const QString& reason, const QJsonValue& data) {
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
        Settle(promise, Lost(reason, data));
    }
    if (wasUp) {
        emit Disconnected(reason);
    }
}

}  // namespace roost
