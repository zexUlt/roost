#pragma once

#include <QByteArray>
#include <QFuture>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QThread>
#include <QtTypes>

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "core/rpc.hpp"
#include "terminal/terminal_session.hpp"

namespace roost {

// A session the window has opened: its channel and the terminal that the IO
// thread feeds.
struct OpenedSession {
    std::uint32_t Channel = 0;
    std::shared_ptr<TerminalSession> Session;
};

using OpenResult = std::expected<OpenedSession, rpc::Error>;

// The window's IO thread (design.md §3): one thread that owns the RpcClient,
// reads the socket, parses the frames and writes the output into the
// terminals of all the window's sessions. The GUI thread never parses output.
//
// The object is used from the thread it lives in, the GUI thread. Each call
// is handed over to the IO thread, and its future finishes there. A call made
// after Stop fails at once with ConnectionLost.
//
// Output is written to a session under its lock in the IO thread. A session
// has at most one Updated signal in flight: after one, the next comes only
// once the GUI has called AcknowledgeUpdate, so a fast stream cannot flood
// the GUI event queue. The GUI acknowledges before it reads the terminal, so
// output that lands in between raises the signal again.
//
// Signals are emitted from the IO thread. Connect GUI receivers with queued
// connections (the default across threads); a slot connected directly runs
// in the IO thread and must not call Stop or delete this object.
class IoThread : public QObject {
    Q_OBJECT

public:
    explicit IoThread(QObject* parent = nullptr);
    // Stops the thread; see Stop.
    ~IoThread() override;

    IoThread(const IoThread&) = delete;
    IoThread& operator=(const IoThread&) = delete;

    // As RpcClient::Connect. Calling it again drops the sessions of the old
    // connection and connects anew.
    QFuture<rpc::Result> Connect(const QString& socketPath);

    // Opens a session channel (session.open). The future finishes once the
    // snapshot frame of the channel has been applied, so the session already
    // shows the screen. It fails with the error of roostd, or with
    // InternalError if the snapshot cannot be restored. After it, the channel
    // raises no Updated until the opener's first AcknowledgeUpdate, which the
    // opener calls before its first read of the session.
    QFuture<OpenResult> Open(const QString& agent, const QString& session,
                             quint16 columns, quint16 rows);

    // Input for a channel; dropped if the channel is not connected.
    void SendInput(std::uint32_t channel, const QByteArray& bytes);

    // session.resize, and on success the same size for the local terminal,
    // under its lock. Raises Updated. A size outside 1..VtTerminal::MAX_SIZE
    // fails with InvalidParams, and nothing is sent.
    QFuture<rpc::Result> Resize(std::uint32_t channel, quint16 columns,
                                quint16 rows);

    // session.close, then the session is dropped from the IO thread. The
    // caller's own shared_ptr keeps the terminal alive.
    QFuture<rpc::Result> Close(std::uint32_t channel);

    // Any other request of the protocol (design.md §4), such as bus.publish or
    // plugins.list: the future yields roostd's answer or error. Not for the
    // methods that carry channel state (session.open, session.resize,
    // session.close): those have their own calls above. Fails with
    // ConnectionLost if not connected. The future finishes in the IO thread;
    // after Stop it finishes at once, in the caller's thread.
    QFuture<rpc::Result> Request(
        const QString& method,
        const QJsonValue& params = QJsonValue::Undefined);

    // Lets the next output of the channel raise Updated again. Call it before
    // reading the terminal; for a new channel, before the first read, since
    // Open leaves the channel without Updated until then. Thread-safe.
    void AcknowledgeUpdate(std::uint32_t channel);

    // Closes the connection, fails the pending futures with ConnectionLost
    // and joins the thread. After it returns no slot of this object runs.
    // Safe to call twice; the destructor calls it.
    void Stop();

signals:
    // The channel's terminal has new content to render.
    void Updated(std::uint32_t channel);
    // The session of the channel ended (the session.closed notification); the
    // channel is gone. Reason is roostd's: "exited with code N" or "evicted".
    void Closed(std::uint32_t channel, const QString& reason);
    // A notification of roostd other than session.closed (which this class
    // handles itself), such as bus.event, in the order received. No order
    // against the futures: an event may be heard before the answer that
    // preceded it on the socket.
    void Notified(const QString& method, const QJsonValue& params);
    // The connection was lost, after the pending futures have failed.
    void Disconnected(const QString& reason);

private:
    class Worker;

    // Runs fn(Worker&) in the IO thread. False if the thread is stopped.
    template <typename TFn>
    bool Post(TFn&& fn);

    QThread Thread_;
    std::unique_ptr<Worker> Worker_;
    // The flag of each channel: set while an Updated is in flight. The IO
    // thread keeps its own copy of the pointer, so the lock is only for the
    // map.
    std::mutex AckMutex_;
    std::unordered_map<std::uint32_t, std::shared_ptr<std::atomic<bool>>> Acks_;
};

}  // namespace roost
