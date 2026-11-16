#pragma once

#include <QFuture>
#include <QJsonObject>
#include <QObject>

#include "core/rpc.hpp"
#include "terminal/io_thread.hpp"

namespace roost::api {

// The plugins roostd has loaded (plugins.list, design.md §4), fetched once
// after the connection comes up and kept. Use it from the GUI thread.
class PluginService : public QObject {
    Q_OBJECT

public:
    explicit PluginService(IoThread& io, QObject* parent = nullptr);

    // Asks roostd for the list and replaces the cache with it; Changed
    // follows. Api calls it once the connection is up. Does nothing while a
    // fetch is in flight.
    void Fetch();

    bool IsLoaded() const;

    // {plugins, problems} as roostd answered; empty until the first fetch
    // has finished.
    const QJsonObject& Plugins() const;

    // The cached answer. Before the first fetch has finished, the answer of
    // that fetch (started here if none is running), so a failed fetch is
    // retried by the next call.
    QFuture<rpc::Result> List();

signals:
    void Changed();

private:
    IoThread& Io_;
    QJsonObject Cache_;
    bool Loaded_ = false;
    bool Fetching_ = false;
    QFuture<rpc::Result> Fetch_;
};

}  // namespace roost::api
