#pragma once

#include <QHash>
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

namespace detail {
class Connection;
}  // namespace detail

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
// are left to the layer above: it names the connection a request came from by
// ConnectionOf(call.Peer), sends it channel frames and notifications, and gets
// the input frames greeted connections send.
//
// A frame sent from inside a handler goes out before the handler's answer.
// To send one after it, answer with a future and finish it first: the answer
// is written from inside QPromise::finish().
//
// A broken stream, an output or snapshot frame from the
// peer, an input frame without a channel number, or a peer that stops reading
// while more than MAX_UNSENT_BYTES wait for it closes the connection.
class Server : public QObject {
    Q_OBJECT

public:
    static constexpr qint64 MAX_UNSENT_BYTES = 64LL * 1024 * 1024;

    // Not reused: an id of a closed connection stays dead.
    using ConnectionId = quint64;

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

    // The connection behind the Call::Peer of a handler's call. Nothing if it
    // is no open connection of this server.
    std::optional<ConnectionId> ConnectionOf(const QObject* peer) const;

    // False if the connection is closed, closes for this frame because its
    // peer does not read, or the bytes do not fit a frame
    // (MAX_CHANNEL_BYTES).
    bool SendToChannel(ConnectionId connection, EFrameKind kind,
                       std::uint32_t channel, QByteArrayView bytes);
    // A JSON-RPC notification; false as for SendToChannel.
    bool Notify(ConnectionId connection, const QString& method,
                const QJsonValue& params);

    // Whether every subscriber can get the event: bus.publish refuses one
    // that does not fit, and so must anything else that publishes what a
    // plugin sent.
    static bool EventFitsAFrame(const QString& name, const QJsonValue& payload);
    // Whether a response with this result fits a frame for any numeric
    // request id; a long string id may still overflow it. A response that
    // does not fit closes the connection.
    static bool AnswerFitsAFrame(const QJsonValue& result);

    qsizetype ConnectionCount() const;

signals:
    // An input frame from a greeted connection. Input that comes before hello
    // is dropped: no channel can be open yet.
    void InputReceived(Server::ConnectionId connection, std::uint32_t channel,
                       const QByteArray& bytes);
    // Once per connection, from the event loop after it closed for any
    // reason. Not while the server is destroyed.
    void ConnectionClosed(Server::ConnectionId connection);

private:
    void Accept();
    void RegisterBuiltins();
    detail::Connection* OpenConnection(ConnectionId connection) const;

    QHash<ConnectionId, detail::Connection*> Connections_;
    ConnectionId NextConnectionId_ = 1;

    rpc::Dispatcher Dispatcher_;
    EventBus Bus_;
    std::unique_ptr<QLockFile> Lock_;
    QLocalServer* Listener_ = nullptr;
};

}  // namespace roost
