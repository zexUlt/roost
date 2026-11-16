#pragma once

#include <QObject>

#include "core/store.hpp"

namespace roost {

class PluginRegistry;
class Server;

// The state of the plugins over the socket (design.md §8): each loaded plugin
// has one JSON document, kept by Store. It adds to the server:
//
//   store.get {plugin}               {document}
//     The plugin's document, {} if it has none yet. InvalidParams if plugin is
//     not a string naming a loaded plugin; StoreFailed, with the store's
//     message, if the file cannot be read or does not parse; the file is left
//     as it is.
//   store.set {plugin, document}     null
//     Replaces the whole document: the last writer wins. InvalidParams if
//     plugin is not a loaded plugin, document is not an object, or the event
//     below would not fit a frame, in which case nothing is written;
//     StoreFailed, with the store's message, if the save fails.
//
// and publishes on the bus, after each successful store.set:
//
//   store.changed {plugin, document}
//
// The server and the registry must outlive it; after it is gone, its methods
// fail with InternalError.
class StoreService : public QObject {
    Q_OBJECT

public:
    StoreService(Store store, Server& server, const PluginRegistry& plugins,
                 QObject* parent = nullptr);
    ~StoreService() override;

    StoreService(const StoreService&) = delete;
    StoreService& operator=(const StoreService&) = delete;

private:
    Store Store_;
    Server& Server_;
    const PluginRegistry& Plugins_;
};

}  // namespace roost
