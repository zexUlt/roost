#include "roostd/store_service.hpp"

#include <QJsonObject>
#include <QPointer>

#include "core/protocol.hpp"
#include "roostd/manifest.hpp"
#include "roostd/plugins.hpp"
#include "roostd/server.hpp"

namespace roost {

namespace {

rpc::Result Fail(rpc::EErrorCode code, const QString& message) {
    return std::unexpected(rpc::Error::FromCode(code, message));
}

rpc::Result InvalidParams(const QString& message) {
    return Fail(rpc::EErrorCode::InvalidParams, message);
}

rpc::Result StoreFailed(const QString& message) {
    return std::unexpected(
        protocol::MakeError(protocol::EErrorCode::StoreFailed, message));
}

const QString CHANGED = QStringLiteral("store.changed");

}  // namespace

StoreService::StoreService(Store store, Server& server, PluginHost& plugins,
                           QObject* parent)
    : QObject(parent),
      Store_(std::move(store)),
      Server_(server),
      Plugins_(plugins.Registry()) {
    // The handlers stay with the server; they find the service gone, if it is.
    const QPointer<StoreService> self(this);
    const auto gone = [] {
        return rpc::Ready(Fail(rpc::EErrorCode::InternalError,
                               QStringLiteral("the store is not available")));
    };
    const bool getting = Server_.Register(
        QStringLiteral("store.get"),
        [self, gone](const rpc::Call& call) -> QFuture<rpc::Result> {
            if (!self) {
                return gone();
            }
            return rpc::Ready(self->Get(call.Params.toObject()));
        });
    Q_ASSERT(getting);
    const bool setting = Server_.Register(
        QStringLiteral("store.set"),
        [self, gone](const rpc::Call& call) -> QFuture<rpc::Result> {
            if (!self) {
                return gone();
            }
            return rpc::Ready(self->Set(call.Params.toObject()));
        });
    Q_ASSERT(setting);
    const bool patching = Server_.Register(
        QStringLiteral("store.patch"),
        [self, gone](const rpc::Call& call) -> QFuture<rpc::Result> {
            if (!self) {
                return gone();
            }
            return rpc::Ready(self->Patch(call.Params.toObject()));
        });
    Q_ASSERT(patching);
    Q_UNUSED(getting);
    Q_UNUSED(setting);
    Q_UNUSED(patching);

    // The backend of a plugin gets the same two methods for its own document.
    const auto own = [self](auto method) {
        return [self, method](const QString& pluginId,
                              const QJsonValue& params) -> rpc::Result {
            if (!self) {
                return Fail(rpc::EErrorCode::InternalError,
                            QStringLiteral("the store is not available"));
            }
            QJsonObject given = params.toObject();
            const QJsonValue plugin = given.value(QStringLiteral("plugin"));
            if (!plugin.isUndefined() && plugin != QJsonValue(pluginId)) {
                return InvalidParams(
                    QStringLiteral("a backend reaches the document of its own "
                                   "plugin only: leave out plugin or give %1")
                        .arg(pluginId));
            }
            given.insert(QStringLiteral("plugin"), pluginId);
            return (self.data()->*method)(given);
        };
    };
    const bool servingGet = plugins.ServeBackends(QStringLiteral("store.get"),
                                                  own(&StoreService::Get));
    const bool servingPatch = plugins.ServeBackends(
        QStringLiteral("store.patch"), own(&StoreService::Patch));
    Q_ASSERT(servingGet);
    Q_ASSERT(servingPatch);
    Q_UNUSED(servingGet);
    Q_UNUSED(servingPatch);
}

StoreService::~StoreService() = default;

rpc::Result StoreService::Get(const QJsonObject& params) {
    const QJsonValue plugin = params.value(QStringLiteral("plugin"));
    if (!plugin.isString() || !HasDocument(plugin.toString())) {
        return InvalidParams(
            QStringLiteral("store.get takes {plugin}: the id of a loaded "
                           "plugin, \"layout\" or \"settings\""));
    }
    const auto document = Store_.Load(plugin.toString());
    if (!document) {
        return StoreFailed(document.error());
    }
    return QJsonObject{{QStringLiteral("document"), *document}};
}

rpc::Result StoreService::Set(const QJsonObject& params) {
    const QJsonValue plugin = params.value(QStringLiteral("plugin"));
    const QJsonValue document = params.value(QStringLiteral("document"));
    if (!plugin.isString() || !HasDocument(plugin.toString()) ||
        !document.isObject()) {
        return InvalidParams(QStringLiteral(
            "store.set takes {plugin, document}: the id of a loaded "
            "plugin, \"layout\" or \"settings\", and a JSON "
            "object"));
    }
    const QJsonObject changed{
        {QStringLiteral("plugin"), plugin},
        {QStringLiteral("document"), document},
    };
    if (!Server::EventFitsAFrame(CHANGED, changed)) {
        return InvalidParams(QStringLiteral(
            "the document is too big to be announced in a frame"));
    }
    const auto saved = Store_.Save(plugin.toString(), document.toObject());
    if (!saved) {
        return StoreFailed(saved.error());
    }
    Server_.Bus().Publish(CHANGED, changed);
    return QJsonValue();
}

// The same steps as Set, on the document as it is now: the event loop runs a
// handler to its end, so the load, the merge and the save never interleave with
// another request, and a patch of one key keeps what another writer put in the
// others.
rpc::Result StoreService::Patch(const QJsonObject& params) {
    const QJsonValue plugin = params.value(QStringLiteral("plugin"));
    const QJsonValue patch = params.value(QStringLiteral("patch"));
    if (!plugin.isString() || !HasDocument(plugin.toString()) ||
        !patch.isObject()) {
        return InvalidParams(QStringLiteral(
            "store.patch takes {plugin, patch}: the id of a loaded "
            "plugin, \"layout\" or \"settings\", and a JSON object "
            "(RFC 7386 merge patch)"));
    }
    auto document = Store_.Load(plugin.toString());
    if (!document) {
        return StoreFailed(document.error());
    }
    ApplyMergePatch(*document, patch.toObject());
    const QJsonObject changed{
        {QStringLiteral("plugin"), plugin},
        {QStringLiteral("document"), *document},
    };
    if (!Server::EventFitsAFrame(CHANGED, changed)) {
        return InvalidParams(QStringLiteral(
            "the document is too big to be announced in a frame"));
    }
    const auto saved = Store_.Save(plugin.toString(), *document);
    if (!saved) {
        return StoreFailed(saved.error());
    }
    Server_.Bus().Publish(CHANGED, changed);
    return QJsonValue();
}

bool StoreService::HasDocument(const QString& id) const {
    return Store::IsWindowDocumentId(id) || Plugins_.Find(id) != nullptr;
}

}  // namespace roost
