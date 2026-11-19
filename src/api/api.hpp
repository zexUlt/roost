#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <chrono>

#include "api/bus_service.hpp"
#include "api/command_service.hpp"
#include "api/dispatcher.hpp"
#include "api/plugin_service.hpp"
#include "api/session_service.hpp"
#include "api/store_service.hpp"
#include "core/rpc.hpp"
#include "terminal/io_thread.hpp"
#include "terminal/warm_terminals.hpp"

Q_DECLARE_METATYPE(roost::rpc::Error)

namespace roost::api {

// The delays of the reconnect, as BackendOptions has them for a backend's
// restart.
struct ApiOptions {
    // The wait after the first failed attempt or lost connection; it doubles
    // with each failed attempt, and a successful hello puts it back.
    std::chrono::milliseconds FirstReconnectDelay{500};
    std::chrono::milliseconds MaxReconnectDelay{30'000};
    // The wait before the sessions are listed again while an agent is
    // unavailable; it doubles with each answer that still has one, up to the
    // maximum, and an answer with none puts it back. The retry has no end
    // while the connection is up and an agent is unavailable (or no list has
    // come); only the wait is capped.
    std::chrono::milliseconds FirstRelistDelay{500};
    std::chrono::milliseconds MaxRelistDelay{30'000};
};

// The root of the window's API (design.md §5, "Один API"): one connection to
// roostd, the IO thread that serves it, the services over it and the
// dispatcher of methods by name. The QML singletons Roost.* and the host of
// the C SDK are thin adapters over this object.
//
// Use it, and everything it owns, from the GUI thread (the thread that made
// it): a call from another thread calls the check handler in a debug build
// (thread_check.hpp). The signals are emitted in that thread too.
//
// One connection at a time, kept up: when it is lost, or an attempt fails, the
// Api tries again on the same socket after FirstReconnectDelay, doubling the
// wait up to MaxReconnectDelay, and never gives up. After a successful hello
// it re-subscribes the listeners of the bus under their own ids, has the
// caches fetched anew and then says Connected. A call that was pending when
// the connection was lost has failed with ConnectionLost and is not sent
// again: it may have been done (commands.run, store.set).
//
// The session channels of the terminals do not survive a loss. The window's
// warm terminals (WarmTerminals, kept here so they outlive a panel) drop the
// hidden ones and, on Connected, open the shown ones again; the Api does not.
class Api : public QObject {
    Q_OBJECT

public:
    explicit Api(ApiOptions options = {}, QObject* parent = nullptr);

    // Connects and says hello; the outcome is one of the signals below. A
    // later call logs a warning and changes nothing, unless roostd refused
    // hello: then it tries again, from the first delay.
    void Connect(const QString& socketPath);
    bool IsConnected() const;

    // The window's own parts, for what is not a by-name method: the terminal
    // panes show sessions through the warm terminals, and the IO thread carries
    // their channels.
    IoThread& Io() { return Io_; }
    WarmTerminals& Terminals() { return Terminals_; }
    BusService& Bus() { return Bus_; }
    CommandService& Commands() { return Commands_; }
    PluginService& Plugins() { return Plugins_; }
    SessionService& Sessions() { return Sessions_; }
    StoreService& Store() { return Store_; }
    Dispatcher& Dispatch() { return Dispatch_; }

signals:
    // After hello succeeded, on the first connection and on each one after a
    // loss; the listeners are subscribed again and the caches are being
    // fetched.
    void Connected();
    // A live connection is lost (once for the loss), or the first attempt
    // failed. Pending calls have failed with ConnectionLost. The attempts that
    // follow are silent until one succeeds.
    void Disconnected(const QString& reason);
    // roostd answered hello with an error: VersionMismatch, or anything else
    // but a lost connection. The attempts stop; Disconnected follows if it
    // has not been said. Connect starts them again.
    void Refused(const roost::rpc::Error& error);

    // An attempt found nobody at the socket: no socket file, or a file that
    // nobody answers on (a roostd that died). Not said for a server that
    // answers and refuses (Refused) or a connection that was lost after it
    // was up. Said again by each such attempt; the retry goes on as before.
    void NoListener();

private:
    // One attempt: connect and say hello.
    void Attempt();
    void OnHello(const rpc::Result& hello);
    void OnDisconnected(const QString& reason);

    const ApiOptions Options_;
    // Declared first, so that it goes last: the services hold a reference.
    IoThread Io_;
    WarmTerminals Terminals_;
    BusService Bus_;
    CommandService Commands_;
    PluginService Plugins_;
    SessionService Sessions_;
    StoreService Store_;
    Dispatcher Dispatch_;
    QTimer RetryTimer_;
    QString SocketPath_;
    // The wait before the next attempt.
    std::chrono::milliseconds Delay_;
    bool Started_ = false;
    bool Connected_ = false;
    // Refused: no attempts until Connect is called.
    bool Refused_ = false;
    // A refused hello is always followed by the Disconnected of its client;
    // that one schedules nothing, even if Connect was called in between.
    bool RefusalTail_ = false;
    // Disconnected has been said since the last Connect or hello.
    bool Reported_ = false;
};

}  // namespace roost::api
