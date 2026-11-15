#pragma once

#include <QJsonValue>
#include <QObject>
#include <QString>

#include <sys/types.h>

#include <expected>
#include <memory>
#include <optional>

#include "core/bus.hpp"
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
// A connection's subscriptions end with it. A broken stream, a frame of a
// kind M1 does not serve, or a peer that stops reading while more than
// MAX_UNSENT_BYTES wait for it closes the connection.
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

    qsizetype ConnectionCount() const;

private:
    void Accept();
    void RegisterBuiltins();

    rpc::Dispatcher Dispatcher_;
    EventBus Bus_;
    std::unique_ptr<QLockFile> Lock_;
    QLocalServer* Listener_ = nullptr;
};

}  // namespace roost
