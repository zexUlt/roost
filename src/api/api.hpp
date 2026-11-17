#pragma once

#include <QObject>
#include <QString>

#include "api/bus_service.hpp"
#include "api/command_service.hpp"
#include "api/dispatcher.hpp"
#include "api/plugin_service.hpp"
#include "api/session_service.hpp"
#include "api/store_service.hpp"
#include "core/rpc.hpp"
#include "terminal/io_thread.hpp"

Q_DECLARE_METATYPE(roost::rpc::Error)

namespace roost::api {

// The root of the window's API (design.md §5, "Один API"): one connection to
// roostd, the IO thread that serves it, the services over it and the
// dispatcher of methods by name. The QML singletons Roost.* and the host of
// the C SDK are thin adapters over this object.
//
// Use it, and everything it owns, from the GUI thread (the thread that made
// it): a call from another thread calls the check handler in a debug build
// (thread_check.hpp). The signals are emitted in that thread too.
//
// One connection, made once: there is no reconnect yet.
class Api : public QObject {
    Q_OBJECT

public:
    explicit Api(QObject* parent = nullptr);

    // Connects and says hello; the outcome is one of the signals below. Only
    // the first call does it: a later one logs a warning and changes nothing.
    void Connect(const QString& socketPath);
    bool IsConnected() const;

    // The window's own parts, for what is not a by-name method: the terminal
    // panes open session channels through the IO thread.
    IoThread& Io() { return Io_; }
    BusService& Bus() { return Bus_; }
    CommandService& Commands() { return Commands_; }
    PluginService& Plugins() { return Plugins_; }
    SessionService& Sessions() { return Sessions_; }
    StoreService& Store() { return Store_; }
    Dispatcher& Dispatch() { return Dispatch_; }

signals:
    // After hello succeeded; the plugin and session lists are being fetched.
    void Connected();
    // The connection is lost or could not be made. Pending calls have failed
    // with ConnectionLost.
    void Disconnected(const QString& reason);
    // roostd answered hello with an error: VersionMismatch, or anything else
    // but a lost connection. Disconnected follows.
    void Refused(const roost::rpc::Error& error);

private:
    // Declared first, so that it goes last: the services hold a reference.
    IoThread Io_;
    BusService Bus_;
    CommandService Commands_;
    PluginService Plugins_;
    SessionService Sessions_;
    StoreService Store_;
    Dispatcher Dispatch_;
    bool Started_ = false;
    bool Connected_ = false;
};

}  // namespace roost::api
