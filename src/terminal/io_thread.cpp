#include "terminal/io_thread.hpp"

#include <QJsonObject>
#include <QMetaObject>
#include <QPromise>
#include <QtLogging>

#include <limits>
#include <utility>

#include "core/protocol.hpp"
#include "core/rpc_client.hpp"
#include "core/vt_terminal.hpp"

namespace roost {

namespace {

template <typename TResult>
using SharedPromise = std::shared_ptr<QPromise<TResult>>;

template <typename TResult>
SharedPromise<TResult> NewPromise() {
    auto promise = std::make_shared<QPromise<TResult>>();
    promise->start();
    return promise;
}

template <typename TResult>
void Settle(QPromise<TResult>& promise, TResult result) {
    promise.addResult(std::move(result));
    promise.finish();
}

rpc::Error Lost(const QString& reason) {
    return protocol::MakeError(protocol::EErrorCode::ConnectionLost, reason);
}

rpc::Error Internal(QString message) {
    return rpc::Error::FromCode(rpc::EErrorCode::InternalError,
                                std::move(message));
}

}  // namespace

// Lives in the IO thread and owns everything that is touched there: the
// client, the sessions and the channels waiting for their snapshot. Its
// methods run only in that thread.
class IoThread::Worker : public QObject {
public:
    explicit Worker(IoThread& owner) : Owner_(owner) {}

    void Connect(const SharedPromise<rpc::Result>& promise,
                 const QString& socketPath) {
        Reset(QStringLiteral("reconnecting"));
        Rpc_ = std::make_unique<RpcClient>();
        connect(Rpc_.get(), &RpcClient::OutputReceived, this,
                &Worker::OnOutput);
        connect(Rpc_.get(), &RpcClient::SnapshotReceived, this,
                &Worker::OnSnapshot);
        connect(Rpc_.get(), &RpcClient::NotificationReceived, this,
                &Worker::OnNotification);
        connect(Rpc_.get(), &RpcClient::Disconnected, this,
                &Worker::OnDisconnected);
        // The future finishes inside the socket handler of the client, which
        // is why a continuation without a context is used: it runs there and
        // then, not later through the event loop.
        Rpc_->Connect(socketPath).then([promise](rpc::Result hello) {
            Settle(*promise, std::move(hello));
        });
    }

    void Open(const SharedPromise<OpenResult>& promise, const QString& agent,
              const QString& session, quint16 columns, quint16 rows) {
        if (!Rpc_) {
            Settle(*promise, OpenResult(std::unexpected(NotConnected())));
            return;
        }
        // The answer {channel} is processed at once, in the same pass over
        // the socket as the snapshot frame behind it (design.md §4), so the
        // channel is registered before its snapshot is read.
        Rpc_->Request(QStringLiteral("session.open"),
                      QJsonObject{{QStringLiteral("agent"), agent},
                                  {QStringLiteral("session"), session},
                                  {QStringLiteral("columns"), columns},
                                  {QStringLiteral("rows"), rows}})
            .then([this, promise](rpc::Result answer) {
                OnOpenAnswer(promise, answer);
            });
    }

    void SendInput(std::uint32_t channel, const QByteArray& bytes) {
        if (Rpc_) {
            Rpc_->SendInput(channel, bytes);
        }
    }

    void Resize(const SharedPromise<rpc::Result>& promise,
                std::uint32_t channel, quint16 columns, quint16 rows) {
        if (!Rpc_) {
            Settle(*promise, rpc::Result(std::unexpected(NotConnected())));
            return;
        }
        // The same bounds as the terminals; checked first, so that roostd's
        // PTY and the local terminal never end up at different sizes.
        if (columns < 1 || rows < 1 || columns > VtTerminal::MAX_SIZE ||
            rows > VtTerminal::MAX_SIZE) {
            Settle(*promise,
                   rpc::Result(std::unexpected(rpc::Error::FromCode(
                       rpc::EErrorCode::InvalidParams,
                       QStringLiteral(
                           "the size must be from 1 to %1 columns and rows")
                           .arg(VtTerminal::MAX_SIZE)))));
            return;
        }
        Rpc_->Request(QStringLiteral("session.resize"),
                      QJsonObject{{QStringLiteral("channel"),
                                   static_cast<qint64>(channel)},
                                  {QStringLiteral("columns"), columns},
                                  {QStringLiteral("rows"), rows}})
            .then([this, promise, channel, columns, rows](rpc::Result answer) {
                if (answer) {
                    const auto it = Sessions_.find(channel);
                    if (it != Sessions_.end()) {
                        const auto resized = Resized(it->second, columns, rows);
                        if (!resized) {
                            Settle(*promise, rpc::Result(std::unexpected(
                                                 Internal(resized.error()))));
                            return;
                        }
                        MarkUpdated(channel, it->second);
                    }
                }
                Settle(*promise, std::move(answer));
            });
    }

    void Close(const SharedPromise<rpc::Result>& promise,
               std::uint32_t channel) {
        if (!Rpc_) {
            Settle(*promise, rpc::Result(std::unexpected(NotConnected())));
            return;
        }
        Rpc_->Request(QStringLiteral("session.close"),
                      QJsonObject{{QStringLiteral("channel"),
                                   static_cast<qint64>(channel)}})
            .then([this, promise, channel](rpc::Result answer) {
                // Whatever roostd said, the channel is not used any more.
                Forget(channel);
                Settle(*promise, std::move(answer));
            });
    }

    void Request(const SharedPromise<rpc::Result>& promise,
                 const QString& method, const QJsonValue& params) {
        if (!Rpc_) {
            Settle(*promise, rpc::Result(std::unexpected(NotConnected())));
            return;
        }
        Rpc_->Request(method, params).then([promise](rpc::Result answer) {
            Settle(*promise, std::move(answer));
        });
    }

    // Closes the connection and fails everything that waits for it.
    void Reset(const QString& reason) {
        if (!Rpc_) {
            return;
        }
        // Nobody hears the client from here on, but its futures still finish.
        Rpc_->disconnect(this);
        Rpc_->Disconnect();
        FailAll(reason);
        Rpc_.reset();
    }

private:
    struct Entry {
        std::shared_ptr<TerminalSession> Session;
        // Set while an Updated is in flight; see IoThread::AcknowledgeUpdate.
        std::shared_ptr<std::atomic<bool>> UpdatePending;
    };

    static rpc::Error NotConnected() {
        return Lost(QStringLiteral("not connected to roostd"));
    }

    static std::expected<void, QString> Resized(const Entry& entry,
                                                quint16 columns, quint16 rows) {
        const auto lock = entry.Session->Lock();
        return entry.Session->Resize(lock, columns, rows);
    }

    void OnOpenAnswer(const SharedPromise<OpenResult>& promise,
                      const rpc::Result& answer) {
        if (!answer) {
            Settle(*promise, OpenResult(std::unexpected(answer.error())));
            return;
        }
        const auto channel =
            rpc::ToInteger(answer->toObject().value(QStringLiteral("channel")));
        if (!channel || *channel <= 0 ||
            *channel > std::numeric_limits<std::uint32_t>::max()) {
            Settle(*promise, OpenResult(std::unexpected(Internal(QStringLiteral(
                                 "session.open answered no channel")))));
            return;
        }
        Opening_.insert_or_assign(static_cast<std::uint32_t>(*channel),
                                  promise);
    }

    void OnSnapshot(std::uint32_t channel, const QByteArray& bytes) {
        const auto opening = Opening_.find(channel);
        if (opening == Opening_.end()) {
            qWarning("snapshot for channel %u, which is not being opened",
                     channel);
            return;
        }
        const SharedPromise<OpenResult> promise = std::move(opening->second);
        Opening_.erase(opening);

        auto session = TerminalSession::FromSnapshot(bytes);
        if (!session) {
            Settle(*promise,
                   OpenResult(std::unexpected(Internal(
                       QStringLiteral("cannot restore the snapshot: ") +
                       session.error()))));
            // roostd would go on streaming into a channel nobody reads.
            Rpc_->Request(QStringLiteral("session.close"),
                          QJsonObject{{QStringLiteral("channel"),
                                       static_cast<qint64>(channel)}});
            return;
        }
        Entry entry{
            .Session = std::shared_ptr<TerminalSession>(std::move(*session)),
            // Pending from the start: no Updated until the opener's first
            // AcknowledgeUpdate, so the GUI never hears of a channel it has
            // not yet been given.
            .UpdatePending = std::make_shared<std::atomic<bool>>(true),
        };
        {
            const std::scoped_lock lock(Owner_.AckMutex_);
            Owner_.Acks_.insert_or_assign(channel, entry.UpdatePending);
        }
        Sessions_.insert_or_assign(channel, entry);
        Settle(*promise, OpenResult(OpenedSession{
                             .Channel = channel,
                             .Session = entry.Session,
                         }));
    }

    void OnOutput(std::uint32_t channel, const QByteArray& bytes) {
        const auto it = Sessions_.find(channel);
        if (it == Sessions_.end()) {
            // roostd sends the snapshot of a channel before any of its output
            // (design.md §4), and the answer that makes the channel known is
            // processed before both. Output for an unknown channel is a
            // protocol violation, or the tail of a channel that was closed or
            // ended: there is no terminal to give it to, so it is dropped.
            qWarning("output for channel %u, which has no session", channel);
            return;
        }
        {
            const auto lock = it->second.Session->Lock();
            it->second.Session->Write(lock, bytes);
        }
        MarkUpdated(channel, it->second);
    }

    void OnNotification(const QString& method, const QJsonValue& params) {
        if (method != QStringLiteral("session.closed")) {
            emit Owner_.Notified(method, params);
            return;
        }
        const QJsonObject object = params.toObject();
        const auto channel =
            rpc::ToInteger(object.value(QStringLiteral("channel")));
        if (!channel || *channel <= 0 ||
            *channel > std::numeric_limits<std::uint32_t>::max()) {
            qWarning("session.closed without a channel");
            return;
        }
        const auto number = static_cast<std::uint32_t>(*channel);
        const QString reason =
            object.value(QStringLiteral("reason")).toString();
        const auto opening = Opening_.find(number);
        if (opening != Opening_.end()) {
            const SharedPromise<OpenResult> promise =
                std::move(opening->second);
            Opening_.erase(opening);
            Settle(*promise,
                   OpenResult(std::unexpected(Internal(
                       QStringLiteral("the session closed: ") + reason))));
        }
        // A channel that never reached the GUI (still opening, or failed to
        // open) is not announced.
        if (Sessions_.contains(number)) {
            Forget(number);
            emit Owner_.Closed(number, reason);
        }
    }

    void OnDisconnected(const QString& reason) {
        FailAll(reason);
        emit Owner_.Disconnected(reason);
    }

    // At most one Updated in flight per channel.
    void MarkUpdated(std::uint32_t channel, const Entry& entry) {
        if (!entry.UpdatePending->exchange(true)) {
            emit Owner_.Updated(channel);
        }
    }

    void Forget(std::uint32_t channel) {
        Sessions_.erase(channel);
        const std::scoped_lock lock(Owner_.AckMutex_);
        Owner_.Acks_.erase(channel);
    }

    void FailAll(const QString& reason) {
        auto opening = std::exchange(Opening_, {});
        for (auto& [channel, promise] : opening) {
            Settle(*promise, OpenResult(std::unexpected(Lost(reason))));
        }
        Sessions_.clear();
        const std::scoped_lock lock(Owner_.AckMutex_);
        Owner_.Acks_.clear();
    }

    IoThread& Owner_;
    std::unique_ptr<RpcClient> Rpc_;
    // Channels that were answered and wait for their snapshot.
    std::unordered_map<std::uint32_t, SharedPromise<OpenResult>> Opening_;
    std::unordered_map<std::uint32_t, Entry> Sessions_;
};

IoThread::IoThread(QObject* parent)
    : QObject(parent), Worker_(std::make_unique<Worker>(*this)) {
    Thread_.setObjectName(QStringLiteral("roost-io"));
    Worker_->moveToThread(&Thread_);
    Thread_.start();
}

IoThread::~IoThread() { Stop(); }

template <typename TFn>
bool IoThread::Post(TFn&& fn) {
    if (!Worker_) {
        return false;
    }
    Worker* worker = Worker_.get();
    QMetaObject::invokeMethod(
        worker, [worker, fn = std::forward<TFn>(fn)]() mutable { fn(*worker); },
        Qt::QueuedConnection);
    return true;
}

QFuture<rpc::Result> IoThread::Connect(const QString& socketPath) {
    auto promise = NewPromise<rpc::Result>();
    const QFuture<rpc::Result> future = promise->future();
    if (!Post([promise, socketPath](Worker& worker) {
            worker.Connect(promise, socketPath);
        })) {
        Settle(*promise, rpc::Result(std::unexpected(Lost(
                             QStringLiteral("the IO thread is stopped")))));
    }
    return future;
}

QFuture<OpenResult> IoThread::Open(const QString& agent, const QString& session,
                                   quint16 columns, quint16 rows) {
    auto promise = NewPromise<OpenResult>();
    const QFuture<OpenResult> future = promise->future();
    if (!Post([promise, agent, session, columns, rows](Worker& worker) {
            worker.Open(promise, agent, session, columns, rows);
        })) {
        Settle(*promise, OpenResult(std::unexpected(Lost(
                             QStringLiteral("the IO thread is stopped")))));
    }
    return future;
}

void IoThread::SendInput(std::uint32_t channel, const QByteArray& bytes) {
    Post(
        [channel, bytes](Worker& worker) { worker.SendInput(channel, bytes); });
}

QFuture<rpc::Result> IoThread::Resize(std::uint32_t channel, quint16 columns,
                                      quint16 rows) {
    auto promise = NewPromise<rpc::Result>();
    const QFuture<rpc::Result> future = promise->future();
    if (!Post([promise, channel, columns, rows](Worker& worker) {
            worker.Resize(promise, channel, columns, rows);
        })) {
        Settle(*promise, rpc::Result(std::unexpected(Lost(
                             QStringLiteral("the IO thread is stopped")))));
    }
    return future;
}

QFuture<rpc::Result> IoThread::Close(std::uint32_t channel) {
    auto promise = NewPromise<rpc::Result>();
    const QFuture<rpc::Result> future = promise->future();
    if (!Post([promise, channel](Worker& worker) {
            worker.Close(promise, channel);
        })) {
        Settle(*promise, rpc::Result(std::unexpected(Lost(
                             QStringLiteral("the IO thread is stopped")))));
    }
    return future;
}

QFuture<rpc::Result> IoThread::Request(const QString& method,
                                       const QJsonValue& params) {
    auto promise = NewPromise<rpc::Result>();
    const QFuture<rpc::Result> future = promise->future();
    if (!Post([promise, method, params](Worker& worker) {
            worker.Request(promise, method, params);
        })) {
        Settle(*promise, rpc::Result(std::unexpected(Lost(
                             QStringLiteral("the IO thread is stopped")))));
    }
    return future;
}

void IoThread::AcknowledgeUpdate(std::uint32_t channel) {
    const std::scoped_lock lock(AckMutex_);
    const auto it = Acks_.find(channel);
    if (it != Acks_.end()) {
        it->second->store(false);
    }
}

void IoThread::Stop() {
    if (!Worker_) {
        return;
    }
    // Queued behind the calls already made, so each of them has been handled
    // when this runs; what it leaves pending it fails.
    QMetaObject::invokeMethod(
        Worker_.get(),
        [worker = Worker_.get()] {
            worker->Reset(QStringLiteral("the IO thread is stopped"));
        },
        Qt::BlockingQueuedConnection);
    Thread_.quit();
    Thread_.wait();
    // The thread is gone: the worker can be destroyed from here, and its
    // queued events with it.
    Worker_.reset();
}

}  // namespace roost
