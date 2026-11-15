#pragma once

#include <QByteArray>
#include <QFuture>
#include <QJsonValue>
#include <QObject>
#include <QPromise>
#include <QString>

#include <unordered_map>

#include "core/frame.hpp"
#include "core/rpc.hpp"

class QLocalSocket;

namespace roost {

// The client end of the roostd socket (design.md §4): the window,
// `roostd --dump` and the tests talk to the daemon through it.
//
// One object is one connection: after it is lost, make a new client. Use it
// from the thread it lives in; its futures finish there, inside its socket's
// signal handlers, so a continuation must not delete the client (deleteLater
// it instead).
class RpcClient : public QObject {
    Q_OBJECT

public:
    explicit RpcClient(QObject* parent = nullptr);
    ~RpcClient() override;

    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;

    // Connects and sends hello with the versions of this build. The future
    // yields hello's result or error: VersionMismatch from a roostd of
    // another build, ConnectionLost if no roostd listens at the path. A
    // refused hello drops the connection.
    QFuture<rpc::Result> Connect(const QString& socketPath);

    // Requests made while the connection is coming up wait for it; after it
    // is lost they fail at once with ConnectionLost. There is no timeout.
    QFuture<rpc::Result> Request(
        const QString& method,
        const QJsonValue& params = QJsonValue::Undefined);

    // Closes the connection. Pending requests fail with ConnectionLost.
    void Disconnect();

    bool IsConnected() const { return State_ == EState::Connected; }

signals:
    void NotificationReceived(const QString& method, const QJsonValue& params);
    // Once, after the pending requests have failed.
    void Disconnected(const QString& reason);

private:
    enum class EState { Idle, Connecting, Connected, Lost };

    void OnConnected();
    void Read();
    void Dispatch(const QJsonValue& message);
    void Lose(const QString& reason);

    QLocalSocket* Socket_;
    EState State_ = EState::Idle;
    FrameDecoder Decoder_;
    QByteArray Unsent_;  // frames written before the socket connected
    std::unordered_map<qint64, QPromise<rpc::Result>> Pending_;
    qint64 NextId_ = 1;
    qint64 HelloId_ = 0;
};

}  // namespace roost
