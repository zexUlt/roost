#include "roostd/session_channels.hpp"

#include <QFuture>
#include <QJsonObject>
#include <QPointer>
#include <QPromise>

#include <limits>
#include <optional>
#include <utility>

#include "core/frame.hpp"
#include "core/rpc.hpp"
#include "roostd/adapter_bridge.hpp"
#include "roostd/plugins.hpp"

namespace roost {

namespace {

constexpr PtySize DEFAULT_SIZE{};

rpc::Error InvalidParamsError(const QString& message) {
    return rpc::Error::FromCode(rpc::EErrorCode::InvalidParams, message);
}

rpc::Error InternalError(const QString& message) {
    return rpc::Error::FromCode(rpc::EErrorCode::InternalError, message);
}

rpc::Result InvalidParams(const QString& message) {
    return std::unexpected(InvalidParamsError(message));
}

rpc::Result Gone() {
    return std::unexpected(
        InternalError(QStringLiteral("session channels are not served")));
}

std::optional<quint16> ToDimension(const QJsonValue& value) {
    const std::optional<qint64> number = rpc::ToInteger(value);
    if (!number || *number < 1 || *number > VtTerminal::MAX_SIZE) {
        return std::nullopt;
    }
    return static_cast<quint16>(*number);
}

std::optional<std::uint32_t> ToChannel(const QJsonValue& value) {
    const std::optional<qint64> number = rpc::ToInteger(value);
    if (!number || *number < 1 ||
        *number > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*number);
}

// A dimension that may be left out; nothing if it is there but bad.
std::optional<std::optional<quint16>> OptionalDimension(
    const QJsonObject& params, const QString& name) {
    const QJsonValue value = params.value(name);
    if (value.isUndefined()) {
        return std::optional<quint16>();
    }
    const std::optional<quint16> dimension = ToDimension(value);
    if (!dimension) {
        return std::nullopt;
    }
    return dimension;
}

QString SizeHint() {
    return QStringLiteral("columns and rows are integers from 1 to %1")
        .arg(VtTerminal::MAX_SIZE);
}

}  // namespace

struct SessionChannels::PendingOpen {
    QPromise<rpc::Result> Promise;
    QPointer<QObject> Peer;
    WarmSessionKey Key;
    PtySize Size;
    // Set if the request gave a size.
    std::optional<PtySize> Requested;
    // How to attach; unset if the session was warm when asked.
    std::optional<PtySpec> Spec;
    std::optional<rpc::Error> Failure;
};

SessionChannels::SessionChannels(Server& server, PluginHost& plugins,
                                 WarmPoolOptions options, QObject* parent)
    : QObject(parent),
      Server_(server),
      Plugins_(plugins),
      Pool_(options, this) {
    // The handlers stay with the server; they find this gone, if it is.
    const QPointer<SessionChannels> self(this);
    using Method = QFuture<rpc::Result> (SessionChannels::*)(const rpc::Call&);
    const auto add = [&](const QString& name, Method method) {
        const bool registered = Server_.Register(
            name,
            [self, method](const rpc::Call& call) -> QFuture<rpc::Result> {
                if (!self) {
                    return rpc::Ready(Gone());
                }
                return (self.data()->*method)(call);
            });
        Q_ASSERT(registered);
        Q_UNUSED(registered);
    };
    add(QStringLiteral("session.open"), &SessionChannels::Open);
    add(QStringLiteral("session.resize"), &SessionChannels::Resize);
    add(QStringLiteral("session.close"), &SessionChannels::Close);
    add(QStringLiteral("session.view"), &SessionChannels::View);

    connect(&Server_, &Server::InputReceived, this, &SessionChannels::OnInput);
    connect(&Server_, &Server::ConnectionClosed, this,
            &SessionChannels::OnConnectionClosed);

    RemovedSubscription_ = Server_.Bus().Subscribe(
        QStringLiteral("session.removed"),
        [this](const QString&, const QJsonValue& payload) {
            OnSessionRemoved(payload);
        });
    Q_ASSERT(RemovedSubscription_);
}

SessionChannels::~SessionChannels() {
    if (RemovedSubscription_) {
        Server_.Bus().Unsubscribe(*RemovedSubscription_);
    }
}

qsizetype SessionChannels::ChannelCount() const {
    qsizetype count = 0;
    for (const ConnectionChannels& channels : Connections_) {
        count += channels.ByNumber.size();
    }
    return count;
}

QFuture<rpc::Result> SessionChannels::Open(const rpc::Call& call) {
    const QJsonObject params = call.Params.toObject();
    const QJsonValue agent = params.value(QStringLiteral("agent"));
    const QJsonValue session = params.value(QStringLiteral("session"));
    if (!agent.isString() || !session.isString()) {
        return rpc::Ready(InvalidParams(QStringLiteral(
            "session.open takes {agent, session, columns?, rows?}: the agent "
            "and the session are strings")));
    }
    const auto columns = OptionalDimension(params, QStringLiteral("columns"));
    const auto rows = OptionalDimension(params, QStringLiteral("rows"));
    if (!columns || !rows || columns->has_value() != rows->has_value()) {
        return rpc::Ready(InvalidParams(
            SizeHint() + QStringLiteral(", given both or neither")));
    }

    auto open = std::make_shared<PendingOpen>();
    open->Peer = call.Peer;
    open->Key = WarmSessionKey{.Agent = agent.toString(),
                               .Session = session.toString()};
    open->Size = PtySize{.Columns = columns->value_or(DEFAULT_SIZE.Columns),
                         .Rows = rows->value_or(DEFAULT_SIZE.Rows)};
    if (columns->has_value()) {
        open->Requested = open->Size;
    }
    open->Promise.start();
    QFuture<rpc::Result> answer = open->Promise.future();

    const auto complete = [self = QPointer<SessionChannels>(this), open] {
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(
            self.data(), [self, open] { self->Complete(open); },
            Qt::QueuedConnection);
    };

    if (Pool_.Find(open->Key) != nullptr) {
        complete();
        return answer;
    }
    AdapterBridge* bridge = Plugins_.BridgeOf(open->Key.Agent);
    if (bridge == nullptr) {
        return rpc::Ready(InvalidParams(
            QStringLiteral("no plugin serves agent %1").arg(open->Key.Agent)));
    }
    // The adapter may answer at once: the last step waits for the event loop
    // all the same.
    bridge->AttachCommand(open->Key.Session)
        .then(QtFuture::Launch::Sync,
              [open, complete](
                  const AdapterBridge::Answer<std::optional<PtySpec>>& attach) {
                  if (!attach) {
                      open->Failure = attach.error();
                  } else if (!attach->has_value()) {
                      open->Failure = InvalidParamsError(
                          QStringLiteral(
                              "session %1 of agent %2 cannot be attached to")
                              .arg(open->Key.Session, open->Key.Agent));
                  } else {
                      open->Spec = **attach;
                  }
                  complete();
              });
    return answer;
}

void SessionChannels::Complete(const std::shared_ptr<PendingOpen>& open) {
    const auto finish = [&open](rpc::Result result) {
        open->Promise.addResult(std::move(result));
        open->Promise.finish();
    };
    if (open->Failure) {
        finish(std::unexpected(*open->Failure));
        return;
    }
    const std::optional<Server::ConnectionId> connection =
        Server_.ConnectionOf(open->Peer.data());
    if (!connection) {
        finish(std::unexpected(
            InternalError(QStringLiteral("the connection is closed"))));
        return;
    }

    // Warm by now, if it was when asked or another open attached first.
    WarmSession* session = Pool_.Find(open->Key);
    if (session != nullptr) {
        if (open->Requested && session->Size() != *open->Requested) {
            if (auto resized = session->Resize(*open->Requested); !resized) {
                finish(std::unexpected(InternalError(resized.error())));
                return;
            }
        }
    } else if (open->Spec) {
        auto opened = Pool_.Open(open->Key, *open->Spec, open->Size);
        if (!opened) {
            finish(std::unexpected(InternalError(opened.error())));
            return;
        }
        session = *opened;
    } else {
        finish(std::unexpected(
            InternalError(QStringLiteral("the session is closed"))));
        return;
    }

    // From the snapshot on, nothing runs that could add output.
    const auto snapshot = session->Snapshot();
    if (!snapshot) {
        finish(std::unexpected(InternalError(snapshot.error())));
        return;
    }
    if (snapshot->size() > MAX_CHANNEL_BYTES) {
        finish(std::unexpected(InternalError(
            QStringLiteral("the snapshot does not fit a frame"))));
        return;
    }
    Pool_.Touch(session);
    const std::uint32_t number = AddChannel(*connection, session);
    // The answer goes out inside finish(), the snapshot after it.
    finish(
        QJsonObject{{QStringLiteral("channel"), static_cast<qint64>(number)}});
    Server_.SendToChannel(*connection, EFrameKind::Snapshot, number, *snapshot);

    const auto it = Connections_.find(*connection);
    if (it == Connections_.end() || !it->ByNumber.contains(number)) {
        return;
    }
    Channel& channel = it->ByNumber[number];
    channel.OutputLink = connect(
        session, &WarmSession::Output, this,
        // A PTY emits about a megabyte at most: one frame.
        [this, id = *connection, number](const QByteArray& bytes) {
            Server_.SendToChannel(id, EFrameKind::Output, number, bytes);
        });
}

std::uint32_t SessionChannels::AddChannel(Server::ConnectionId connection,
                                          WarmSession* session) {
    ConnectionChannels& channels = Connections_[connection];
    const std::uint32_t number = ++channels.Last;
    Channel& channel = channels.ByNumber[number];
    channel.Session = session;
    channel.ClosedLink =
        connect(session, &WarmSession::Closed, this,
                [this, connection, number](const QString& reason) {
                    OnSessionClosed(connection, number, reason);
                });
    return number;
}

bool SessionChannels::RemoveChannel(Server::ConnectionId connection,
                                    std::uint32_t number) {
    const auto it = Connections_.find(connection);
    if (it == Connections_.end()) {
        return false;
    }
    const auto channel = it->ByNumber.find(number);
    if (channel == it->ByNumber.end()) {
        return false;
    }
    disconnect(channel->OutputLink);
    disconnect(channel->ClosedLink);
    it->ByNumber.erase(channel);
    return true;
}

WarmSession* SessionChannels::SessionOf(Server::ConnectionId connection,
                                        std::uint32_t number) const {
    const auto it = Connections_.constFind(connection);
    if (it == Connections_.cend()) {
        return nullptr;
    }
    const auto channel = it->ByNumber.constFind(number);
    return channel == it->ByNumber.cend() ? nullptr : channel->Session.data();
}

QFuture<rpc::Result> SessionChannels::Resize(const rpc::Call& call) {
    const QJsonObject params = call.Params.toObject();
    const auto number = ToChannel(params.value(QStringLiteral("channel")));
    const auto columns = ToDimension(params.value(QStringLiteral("columns")));
    const auto rows = ToDimension(params.value(QStringLiteral("rows")));
    if (!number || !columns || !rows) {
        return rpc::Ready(InvalidParams(
            QStringLiteral("session.resize takes {channel, columns, rows}: "
                           "a channel number; %1")
                .arg(SizeHint())));
    }
    const auto connection = Server_.ConnectionOf(call.Peer);
    WarmSession* session =
        connection ? SessionOf(*connection, *number) : nullptr;
    if (session == nullptr) {
        return rpc::Ready(
            InvalidParams(QStringLiteral("no channel %1").arg(*number)));
    }
    if (auto resized =
            session->Resize(PtySize{.Columns = *columns, .Rows = *rows});
        !resized) {
        // The size is checked, so this is a closed session or the PTY.
        return rpc::Ready(std::unexpected(InternalError(resized.error())));
    }
    Pool_.Touch(session);
    return rpc::Ready(QJsonValue());
}

QFuture<rpc::Result> SessionChannels::Close(const rpc::Call& call) {
    const auto number =
        ToChannel(call.Params.toObject().value(QStringLiteral("channel")));
    if (!number) {
        return rpc::Ready(InvalidParams(
            QStringLiteral("session.close takes {channel}: a channel number")));
    }
    const auto connection = Server_.ConnectionOf(call.Peer);
    if (!connection || !RemoveChannel(*connection, *number)) {
        return rpc::Ready(
            InvalidParams(QStringLiteral("no channel %1").arg(*number)));
    }
    return rpc::Ready(QJsonValue());
}

QFuture<rpc::Result> SessionChannels::View(const rpc::Call& call) {
    const auto number =
        ToChannel(call.Params.toObject().value(QStringLiteral("channel")));
    if (!number) {
        return rpc::Ready(InvalidParams(
            QStringLiteral("session.view takes {channel}: a channel number")));
    }
    const auto connection = Server_.ConnectionOf(call.Peer);
    WarmSession* session =
        connection ? SessionOf(*connection, *number) : nullptr;
    if (session == nullptr) {
        return rpc::Ready(
            InvalidParams(QStringLiteral("no channel %1").arg(*number)));
    }
    Pool_.Touch(session);
    return rpc::Ready(QJsonValue());
}

void SessionChannels::OnInput(Server::ConnectionId connection,
                              std::uint32_t number, const QByteArray& bytes) {
    WarmSession* session = SessionOf(connection, number);
    if (session == nullptr) {
        return;
    }
    session->Write(bytes);
    Pool_.Touch(session);
}

void SessionChannels::OnSessionClosed(Server::ConnectionId connection,
                                      std::uint32_t number,
                                      const QString& reason) {
    if (!RemoveChannel(connection, number)) {
        return;
    }
    Server_.Notify(connection, QStringLiteral("session.closed"),
                   QJsonObject{
                       {QStringLiteral("channel"), static_cast<qint64>(number)},
                       {QStringLiteral("reason"), reason},
                   });
}

void SessionChannels::OnConnectionClosed(Server::ConnectionId connection) {
    const auto it = Connections_.find(connection);
    if (it == Connections_.end()) {
        return;
    }
    for (const Channel& channel : std::as_const(it->ByNumber)) {
        disconnect(channel.OutputLink);
        disconnect(channel.ClosedLink);
    }
    Connections_.erase(it);
}

void SessionChannels::OnSessionRemoved(const QJsonValue& payload) {
    const QJsonObject object = payload.toObject();
    const QJsonValue agent = object.value(QStringLiteral("agent"));
    const QJsonValue id = object.value(QStringLiteral("id"));
    if (!agent.isString() || !id.isString()) {
        return;
    }
    // Closed tells the channels of the session and forgets it.
    Pool_.Evict(
        WarmSessionKey{.Agent = agent.toString(), .Session = id.toString()},
        QStringLiteral("removed"));
}

}  // namespace roost
