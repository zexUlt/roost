#pragma once

#include <QFuture>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QStringList>

#include <expected>
#include <map>

#include "api/bus_service.hpp"
#include "core/rpc.hpp"

namespace roost::api {

class CommandService;
class PluginService;
class SessionService;
class StoreService;

// The methods of design.md §4 that a plugin may call, by name: the one table
// that the QML singletons and the host of the C SDK are adapters over. It is
// rpc::Dispatcher's method table without the bytes: a name, a JSON value of
// params, a future of rpc::Result.
//
// Exposed: sessions.list, plugins.list, store.get, store.set, commands.run,
// bus.publish. Not exposed: hello and session.open, session.resize and
// session.close, which belong to the window itself. Subscribing takes a
// callback, so it is a pair of methods here and not a by-name call.
//
// The methods that have a cache go through its service: sessions.list always
// asks roostd and refreshes the rows, store.get answers from the documents
// that are warm, store.set leaves the cache to the event that follows it.
//
// Use it from the GUI thread.
class Dispatcher : public QObject {
    Q_OBJECT

public:
    Dispatcher(BusService& bus, CommandService& commands,
               PluginService& plugins, SessionService& sessions,
               StoreService& store, QObject* parent = nullptr);

    // As rpc::Dispatcher::Register: false if the method is taken, or the name
    // is reserved by JSON-RPC (starts with "rpc.").
    [[nodiscard]] bool Register(const QString& method, rpc::Handler handler);

    // The names of the methods, sorted.
    QStringList Methods() const;

    // The future finishes in the thread of `context` (this object's, if
    // null), and finishes canceled if the context is destroyed first. An
    // unknown name is MethodNotFound; errors of roostd, whatever their code,
    // pass unchanged. Null params are no params.
    QFuture<rpc::Result> Call(const QString& method, const QJsonValue& params,
                              QObject* context);

    // BusService::Subscribe and BusService::Unsubscribe.
    std::expected<BusService::LocalId, rpc::Error> Subscribe(
        const QString& pattern, const QObject* context,
        BusService::Listener listener);
    void Unsubscribe(BusService::LocalId id);

private:
    void RegisterBuiltins(CommandService& commands, PluginService& plugins,
                          SessionService& sessions, StoreService& store);

    BusService& Bus_;
    std::map<QString, rpc::Handler> Handlers_;
};

}  // namespace roost::api
