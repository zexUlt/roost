#include "roostd/server.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <limits>
#include <memory>
#include <set>
#include <utility>

#include "core/frame.hpp"
#include "core/protocol.hpp"
#include "core/version.hpp"

namespace roost {

namespace {

// What a subscriber gets for an event.
QByteArray EventMessage(qint64 subscription, const QString& name,
                        const QJsonValue& payload) {
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                   {QStringLiteral("method"), QStringLiteral("bus.event")},
                   {QStringLiteral("params"),
                    QJsonObject{
                        {QStringLiteral("subscription"), subscription},
                        {QStringLiteral("name"), name},
                        {QStringLiteral("payload"), payload},
                    }},
               })
        .toJson(QJsonDocument::Compact);
}

bool FitsAFrame(qsizetype size) {
    // The length counts the kind byte too.
    return size < static_cast<qsizetype>(MAX_FRAME_LENGTH);
}

}  // namespace

namespace detail {

// One accepted socket: its frame decoder, its handshake state and the bus
// subscriptions it made. Deletes itself when the socket goes away.
class Connection : public QObject {
    Q_OBJECT

public:
    Connection(QLocalSocket* socket, const rpc::Dispatcher& dispatcher,
               EventBus& bus, QObject* parent)
        : QObject(parent), Socket_(socket), Dispatcher_(dispatcher), Bus_(bus) {
        Socket_->setParent(this);
        connect(Socket_, &QLocalSocket::readyRead, this, &Connection::Read);
        connect(Socket_, &QLocalSocket::disconnected, this,
                &QObject::deleteLater);
        // Bytes that came with the connection, before the signal was wired.
        Read();
    }

    ~Connection() override {
        for (const EventBus::SubscriptionId id : Subscriptions_) {
            Bus_.Unsubscribe(id);
        }
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool Greeted() const { return Greeted_; }
    void Greet() { Greeted_ = true; }

    std::optional<EventBus::SubscriptionId> Subscribe(const QString& pattern) {
        // The listener needs its own id, which exists only after Subscribe.
        auto self = std::make_shared<EventBus::SubscriptionId>(0);
        const auto id = Bus_.Subscribe(
            pattern,
            [this, self](const QString& name, const QJsonValue& payload) {
                Send(EventMessage(static_cast<qint64>(*self), name, payload));
            });
        if (id) {
            *self = *id;
            Subscriptions_.insert(*id);
        }
        return id;
    }

    bool Unsubscribe(EventBus::SubscriptionId id) {
        if (Subscriptions_.erase(id) == 0) {
            return false;
        }
        return Bus_.Unsubscribe(id);
    }

private:
    void Read() {
        Decoder_.Feed(Socket_->readAll());
        while (!Closed_) {
            std::optional<Frame> frame = Decoder_.Next();
            if (!frame) {
                break;
            }
            if (frame->Kind != EFrameKind::Rpc) {
                Close(QStringLiteral("frames of kind %1 are not served")
                          .arg(static_cast<int>(frame->Kind)));
                return;
            }
            // Dropped with the connection if it closes first.
            Dispatcher_.Handle(frame->Body, this)
                .then(this, [this](const QByteArray& response) {
                    if (!response.isEmpty()) {
                        Send(response);
                    }
                });
        }
        if (Decoder_.Failed()) {
            Close(Decoder_.Error());
        }
    }

    void Send(QByteArrayView body) {
        if (Closed_) {
            return;
        }
        const std::optional<QByteArray> frame =
            EncodeFrame(EFrameKind::Rpc, body);
        if (!frame) {
            Close(QStringLiteral("a message of %1 bytes does not fit a frame")
                      .arg(body.size()));
            return;
        }
        Socket_->write(*frame);
        if (Socket_->bytesToWrite() > Server::MAX_UNSENT_BYTES) {
            Close(QStringLiteral("the peer does not read its messages"));
        }
    }

    // Drops the connection; the object goes once control is back in the event
    // loop, so a caller up the stack never sees it vanish.
    void Close(const QString& reason) {
        if (Closed_) {
            return;
        }
        Closed_ = true;
        qWarning("roostd: closing a connection: %s", qUtf8Printable(reason));
        Socket_->abort();
        deleteLater();
    }

    QLocalSocket* Socket_;
    const rpc::Dispatcher& Dispatcher_;
    EventBus& Bus_;
    FrameDecoder Decoder_;
    std::set<EventBus::SubscriptionId> Subscriptions_;
    bool Greeted_ = false;
    bool Closed_ = false;
};

}  // namespace detail

namespace {

using detail::Connection;

using rpc::ToInteger;

rpc::Result InvalidParams(const QString& message) {
    return std::unexpected(
        rpc::Error::FromCode(rpc::EErrorCode::InvalidParams, message));
}

rpc::Result NoConnection() {
    return std::unexpected(rpc::Error::FromCode(
        rpc::EErrorCode::InternalError,
        QStringLiteral("Internal error: the call came from no connection")));
}

std::expected<void, QString> PrepareDirectory(const QString& directory) {
    const QByteArray path = QFile::encodeName(directory);
    if (::mkdir(path.constData(), S_IRWXU) != 0 && errno != EEXIST) {
        return std::unexpected(QStringLiteral("cannot create %1: %2")
                                   .arg(directory, qt_error_string(errno)));
    }
    struct stat info{};
    if (::lstat(path.constData(), &info) != 0) {
        return std::unexpected(QStringLiteral("cannot inspect %1: %2")
                                   .arg(directory, qt_error_string(errno)));
    }
    // lstat, so a symlink planted in place of the directory is refused too.
    if (!S_ISDIR(info.st_mode)) {
        return std::unexpected(
            QStringLiteral("%1 is not a directory").arg(directory));
    }
    if (info.st_uid != ::geteuid()) {
        return std::unexpected(
            QStringLiteral("%1 belongs to another user").arg(directory));
    }
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected(
            QStringLiteral("%1 is open to other users (mode %2); chmod 700 it")
                .arg(directory)
                .arg(info.st_mode & ACCESSPERMS, 3, 8, QChar(u'0')));
    }
    return {};
}

}  // namespace

std::optional<uid_t> PeerUid(qintptr descriptor) {
    const int fd = static_cast<int>(descriptor);
#if defined(__APPLE__)
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(fd, &uid, &gid) != 0) {
        return std::nullopt;
    }
    return uid;
#else
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 ||
        length != sizeof(credentials)) {
        return std::nullopt;
    }
    return credentials.uid;
#endif
}

Server::Server(QObject* parent) : QObject(parent) { RegisterBuiltins(); }

Server::~Server() {
    // The socket file goes before the lock does: a roostd that takes the lock
    // next must not have its fresh socket removed by this one.
    if (Listener_ != nullptr) {
        Listener_->close();
    }
    // Connections use the dispatcher and the bus, which are gone by the time
    // QObject deletes its children.
    qDeleteAll(findChildren<Connection*>(Qt::FindDirectChildrenOnly));
}

std::expected<void, QString> Server::Listen(const QString& socketPath) {
    if (Listener_ != nullptr) {
        return std::unexpected(QStringLiteral("already listening"));
    }
    if (!QDir::isAbsolutePath(socketPath)) {
        return std::unexpected(
            QStringLiteral("socket path \"%1\" is not absolute")
                .arg(socketPath));
    }
    constexpr qsizetype maxPath = sizeof(sockaddr_un{}.sun_path) - 1;
    if (QFile::encodeName(socketPath).size() > maxPath) {
        return std::unexpected(
            QStringLiteral("socket path %1 is longer than %2 bytes")
                .arg(socketPath)
                .arg(maxPath));
    }
    if (auto ready = PrepareDirectory(QFileInfo(socketPath).absolutePath());
        !ready) {
        return ready;
    }

    const QString lockPath = socketPath + QStringLiteral(".lock");
    auto lock = std::make_unique<QLockFile>(lockPath);
    // Held for the life of the daemon, so age says nothing; a lock whose
    // process is gone is still taken over.
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) {
        if (lock->error() == QLockFile::LockFailedError) {
            qint64 pid = 0;
            QString host;
            QString application;
            lock->getLockInfo(&pid, &host, &application);
            return std::unexpected(
                QStringLiteral("another roostd (pid %1) is serving %2")
                    .arg(pid)
                    .arg(socketPath));
        }
        return std::unexpected(QStringLiteral("cannot lock %1").arg(lockPath));
    }

    // Whatever is left at the path belongs to a roostd that died: the lock
    // says no other one runs.
    QLocalServer::removeServer(socketPath);
    auto* listener = new QLocalServer(this);
    listener->setSocketOptions(QLocalServer::UserAccessOption);
    if (!listener->listen(socketPath)) {
        const QString error = listener->errorString();
        delete listener;
        return std::unexpected(
            QStringLiteral("cannot listen on %1: %2").arg(socketPath, error));
    }
    connect(listener, &QLocalServer::newConnection, this, &Server::Accept);
    Lock_ = std::move(lock);
    Listener_ = listener;
    return {};
}

bool Server::Register(const QString& method, rpc::Handler handler) {
    return Dispatcher_.Register(
        method, [handler = std::move(handler)](const rpc::Call& call) {
            const auto* connection = qobject_cast<Connection*>(call.Peer);
            if (connection == nullptr || !connection->Greeted()) {
                return rpc::Ready(std::unexpected(
                    protocol::MakeError(protocol::EErrorCode::HandshakeRequired,
                                        QStringLiteral("call hello first"))));
            }
            return handler(call);
        });
}

qsizetype Server::ConnectionCount() const {
    return findChildren<Connection*>(Qt::FindDirectChildrenOnly).size();
}

void Server::Accept() {
    while (QLocalSocket* socket = Listener_->nextPendingConnection()) {
        const std::optional<uid_t> uid = PeerUid(socket->socketDescriptor());
        if (!uid || *uid != ::geteuid()) {
            qWarning("roostd: refused a connection from uid %lld",
                     uid ? static_cast<long long>(*uid) : -1LL);
            socket->abort();
            socket->deleteLater();
            continue;
        }
        new Connection(socket, Dispatcher_, Bus_, this);
    }
}

// The longest id makes the longest message.
bool Server::EventFitsAFrame(const QString& name, const QJsonValue& payload) {
    return FitsAFrame(
        EventMessage(std::numeric_limits<qint64>::max(), name, payload).size());
}

// Numeric ids pass through a double: the longest one makes the longest
// response.
bool Server::AnswerFitsAFrame(const QJsonValue& result) {
    return FitsAFrame(
        QJsonDocument(
            QJsonObject{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), std::numeric_limits<double>::lowest()},
                {QStringLiteral("result"), result},
            })
            .toJson(QJsonDocument::Compact)
            .size());
}

void Server::RegisterBuiltins() {
    const bool registered =
        Dispatcher_.Register(
            QStringLiteral("hello"),
            [](const rpc::Call& call) {
                auto* connection = qobject_cast<Connection*>(call.Peer);
                if (connection == nullptr) {
                    return rpc::Ready(NoConnection());
                }
                const QJsonObject params = call.Params.toObject();
                const std::optional<qint64> version =
                    ToInteger(params.value(QStringLiteral("protocol")));
                const QJsonValue ghosttyVt =
                    params.value(QStringLiteral("ghosttyVt"));
                if (!version || !ghosttyVt.isString()) {
                    return rpc::Ready(InvalidParams(QStringLiteral(
                        "hello takes {protocol: integer, ghosttyVt: string}")));
                }
                const QString ours = QString::fromStdString(GhosttyVtVersion());
                QJsonObject answer{
                    {QStringLiteral("protocol"), protocol::VERSION},
                    {QStringLiteral("ghosttyVt"), ours},
                };
                if (*version != protocol::VERSION ||
                    ghosttyVt.toString() != ours) {
                    return rpc::Ready(std::unexpected(protocol::MakeError(
                        protocol::EErrorCode::VersionMismatch,
                        QStringLiteral("roostd speaks protocol %1 with "
                                       "libghostty-vt %2; restart roostd")
                            .arg(protocol::VERSION)
                            .arg(ours),
                        answer)));
                }
                connection->Greet();
                answer.insert(QStringLiteral("roostd"),
                              QString::fromStdString(VersionString()));
                return rpc::Ready(answer);
            }) &&
        Register(
            QStringLiteral("bus.subscribe"),
            [](const rpc::Call& call) {
                auto* connection = qobject_cast<Connection*>(call.Peer);
                const QJsonValue pattern =
                    call.Params.toObject().value(QStringLiteral("pattern"));
                const auto id = pattern.isString()
                                    ? connection->Subscribe(pattern.toString())
                                    : std::nullopt;
                if (!id) {
                    return rpc::Ready(InvalidParams(QStringLiteral(
                        "bus.subscribe takes {pattern}: an event name, a "
                        "prefix ending in \".*\" or \"*\"")));
                }
                return rpc::Ready(QJsonObject{
                    {QStringLiteral("subscription"), static_cast<qint64>(*id)},
                });
            }) &&
        Register(QStringLiteral("bus.unsubscribe"),
                 [](const rpc::Call& call) {
                     auto* connection = qobject_cast<Connection*>(call.Peer);
                     const std::optional<qint64> id =
                         ToInteger(call.Params.toObject().value(
                             QStringLiteral("subscription")));
                     if (!id || *id < 0 ||
                         !connection->Unsubscribe(
                             static_cast<EventBus::SubscriptionId>(*id))) {
                         return rpc::Ready(InvalidParams(QStringLiteral(
                             "bus.unsubscribe takes {subscription}: an id this "
                             "connection got from bus.subscribe")));
                     }
                     return rpc::Ready(QJsonValue());
                 }) &&
        Register(QStringLiteral("bus.publish"), [this](const rpc::Call& call) {
            const QJsonObject params = call.Params.toObject();
            const QJsonValue name = params.value(QStringLiteral("name"));
            const QJsonValue given = params.value(QStringLiteral("payload"));
            const QJsonValue payload =
                given.isUndefined() ? QJsonValue() : given;
            if (!name.isString() || !EventBus::IsValidName(name.toString())) {
                return rpc::Ready(InvalidParams(QStringLiteral(
                    "bus.publish takes {name, payload}: a dot-separated event "
                    "name and any JSON value")));
            }
            // Refused here rather than dropping the subscribers it would not
            // reach.
            if (!EventFitsAFrame(name.toString(), payload)) {
                return rpc::Ready(InvalidParams(QStringLiteral(
                    "bus.publish: the event does not fit a frame")));
            }
            Bus_.Publish(name.toString(), payload);
            return rpc::Ready(QJsonValue());
        });
    Q_ASSERT(registered);
    Q_UNUSED(registered);
}

}  // namespace roost

#include "server.moc"
