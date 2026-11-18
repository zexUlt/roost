#pragma once

#include <QByteArray>
#include <QFuture>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>

#include "roostd/server.hpp"
#include "roostd/warm_pool.hpp"

namespace roost {

class PluginHost;

// The session channels of design.md §4, over the warm pool of §3. It adds to
// the server:
//
//   session.open {agent, session, columns?, rows?}  {channel}
//     Opens a channel of the session of the agent, attaching to it if it is
//     not warm. columns and rows are from 1 to VtTerminal::MAX_SIZE, given
//     both or neither, 80x24 by default; a size given for a warm session
//     resizes it if it differs. The client gets the answer, then a snapshot
//     frame of the terminal, then the output frames that follow it, without a
//     gap or a repeat between the snapshot and the output. InvalidParams if no
//     plugin serves the agent, the adapter says the session cannot be attached
//     to or the parameters are bad; an error of the adapter comes through as it
//     is; InternalError if the program does not start, or the session or the
//     connection is gone before the channel opens.
//   session.resize {channel, columns, rows}         null
//     InvalidParams for a bad size or an unknown channel; InternalError if the
//     session or its PTY cannot be resized.
//   session.close {channel}                         null
//     The channel only: the session stays warm and nothing is notified.
//   session.view {channel}                          null
//     Marks the session of the channel as viewed now, for the pool's
//     eviction order; InvalidParams for an unknown channel. Nobody is
//     notified and the size does not change.
//
// and, as notification session.closed {channel, reason}, tells the client of
// a channel that its session has closed: the program exited, or the pool
// evicted it (WarmSession::Closed); the channel is gone then. The reason is
// "evicted" for the least recently viewed session and "removed" when the
// daemon's bus announced session.removed {agent, id} for it.
//
// The size belongs to the session, not to a channel: a size given by
// session.open or session.resize is the size of the session for all of its
// channels, and the other channels are not told.
//
// A channel belongs to the connection that opened it and is named by a number
// counted from 1 per connection, never reused; another connection's number
// is unknown. Input frames of a channel go to its session; input for an
// unknown channel is dropped. A connection's channels end with it.
//
// The server and the plugin host must outlive it: after it is gone, its
// methods fail with InternalError.
class SessionChannels : public QObject {
    Q_OBJECT

public:
    SessionChannels(Server& server, PluginHost& plugins,
                    WarmPoolOptions options = {}, QObject* parent = nullptr);

    ~SessionChannels() override;

    SessionChannels(const SessionChannels&) = delete;
    SessionChannels& operator=(const SessionChannels&) = delete;

    WarmPool& Pool() { return Pool_; }

    // The channels of all connections.
    qsizetype ChannelCount() const;

private:
    struct PendingOpen;

    struct Channel {
        QPointer<WarmSession> Session;
        QMetaObject::Connection OutputLink;
        QMetaObject::Connection ClosedLink;
    };

    struct ConnectionChannels {
        std::uint32_t Last = 0;
        QHash<std::uint32_t, Channel> ByNumber;
    };

    QFuture<rpc::Result> Open(const rpc::Call& call);
    QFuture<rpc::Result> Resize(const rpc::Call& call);
    QFuture<rpc::Result> Close(const rpc::Call& call);
    QFuture<rpc::Result> View(const rpc::Call& call);

    // The last step of Open, always from the event loop: the answer reaches
    // the client only once the dispatcher is waiting for it.
    void Complete(const std::shared_ptr<PendingOpen>& open);
    std::uint32_t AddChannel(Server::ConnectionId connection,
                             WarmSession* session);
    // False if there was no such channel.
    bool RemoveChannel(Server::ConnectionId connection, std::uint32_t number);
    WarmSession* SessionOf(Server::ConnectionId connection,
                           std::uint32_t number) const;
    void OnInput(Server::ConnectionId connection, std::uint32_t number,
                 const QByteArray& bytes);
    void OnSessionClosed(Server::ConnectionId connection, std::uint32_t number,
                         const QString& reason);
    void OnConnectionClosed(Server::ConnectionId connection);
    // The bus event session.removed {agent, id}: the session is gone from its
    // agent, so the warm one is evicted with the reason "removed".
    void OnSessionRemoved(const QJsonValue& payload);

    Server& Server_;
    PluginHost& Plugins_;
    WarmPool Pool_;
    QHash<Server::ConnectionId, ConnectionChannels> Connections_;
    std::optional<EventBus::SubscriptionId> RemovedSubscription_;
};

}  // namespace roost
