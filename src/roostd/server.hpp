#pragma once

#include <QJsonValue>
#include <QObject>
#include <QString>

#include <sys/types.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

#include "core/bus.hpp"
#include "core/frame.hpp"
#include "core/rpc.hpp"

class QLocalServer;
class QLockFile;

namespace roost {

// The uid of the process on the other end of a connected unix socket:
// getpeereid on macOS, SO_PEERCRED on Linux. Nothing if the system cannot
// tell.
std::optional<uid_t> PeerUid(qintptr descriptor);

// The socket side of roostd (design.md §4, §12): accepts connections of the
// same user only, cuts their streams into frames and answers JSON-RPC through
// one method table.
//
// A connection must greet first. Until its hello succeeds, every other method
// answers HandshakeRequired. Built in:
//
//   hello {protocol, ghosttyVt} -> {protocol, ghosttyVt, roostd}
//     VersionMismatch, with the server's {protocol, ghosttyVt} as data, if
//     either differs.
//   bus.subscribe {pattern} -> {subscription}
//     Events then arrive as notifications
//     bus.event {subscription, name, payload}.
//   bus.unsubscribe {subscription} -> null
//     Only the connection's own subscriptions.
//   bus.publish {name, payload?} -> null
//
// A connection's subscriptions end with it. Session channels (design.md §4)
// are left to the layer above: it sends channel frames and notifications to
// the connection a request came from and gets the input frames greeted
// connections send. A broken stream, an output or snapshot frame from the
// peer, an input frame without a channel number, or a peer that stops reading
// while more than MAX_UNSENT_BYTES wait for it closes the connection.
class Server : public QObject {
    Q_OBJECT

public:
    static constexpr qint64 MAX_UNSENT_BYTES = 64LL * 1024 * 1024;

    explicit Server(QObject* parent = nullptr);
    ~Server() override;

    // Prepares the socket directory, takes the single-instance lock next to
    // the socket and listens. The directory is created with mode 0700 if
    // missing; an existing one must belong to this user and be closed to
    // everyone else. A socket left by a dead roostd is replaced; a live one is
    // an error.
    std::expected<void, QString> Listen(const QString& socketPath);

    // Adds a method that answers greeted connections only. False if the name
    // is taken or reserved.
    [[nodiscard]] bool Register(const QString& method, rpc::Handler handler);

    EventBus& Bus() { return Bus_; }

    // To the connection that is the Call::Peer of a request it made; its
    // destroyed() says it is gone. False if the peer is no open connection of
    // this server or the bytes do not fit a frame (MAX_CHANNEL_BYTES).
    bool SendToChannel(QObject* peer, EFrameKind kind, std::uint32_t channel,
                       QByteArrayView bytes);
    // A JSON-RPC notification; false as for SendToChannel.
    bool Notify(QObject* peer, const QString& method, const QJsonValue& params);

    qsizetype ConnectionCount() const;

signals:
    // An input frame from a greeted connection. Input that comes before hello
    // is dropped: no channel can be open yet.
    void InputReceived(QObject* peer, std::uint32_t channel,
                       const QByteArray& bytes);

private:
    void Accept();
    void RegisterBuiltins();

    rpc::Dispatcher Dispatcher_;
    EventBus Bus_;
    std::unique_ptr<QLockFile> Lock_;
    QLocalServer* Listener_ = nullptr;
};

}  // namespace roost
