#include "roostd/store_service.hpp"

#include <QJsonObject>
#include <QPointer>

#include "core/protocol.hpp"
#include "roostd/manifest.hpp"
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

StoreService::StoreService(Store store, Server& server,
                           const PluginRegistry& plugins, QObject* parent)
    : QObject(parent),
      Store_(std::move(store)),
      Server_(server),
      Plugins_(plugins) {
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
            const QJsonValue plugin =
                call.Params.toObject().value(QStringLiteral("plugin"));
            if (!plugin.isString() || !self->HasDocument(plugin.toString())) {
                return rpc::Ready(InvalidParams(
                    QStringLiteral("store.get takes {plugin}: the id of a "
                                   "loaded plugin, \"layout\" or "
                                   "\"settings\"")));
            }
            const auto document = self->Store_.Load(plugin.toString());
            if (!document) {
                return rpc::Ready(StoreFailed(document.error()));
            }
            return rpc::Ready(
                QJsonObject{{QStringLiteral("document"), *document}});
        });
    Q_ASSERT(getting);
    const bool setting = Server_.Register(
        QStringLiteral("store.set"),
        [self, gone](const rpc::Call& call) -> QFuture<rpc::Result> {
            if (!self) {
                return gone();
            }
            const QJsonObject params = call.Params.toObject();
            const QJsonValue plugin = params.value(QStringLiteral("plugin"));
            const QJsonValue document =
                params.value(QStringLiteral("document"));
            if (!plugin.isString() || !self->HasDocument(plugin.toString()) ||
                !document.isObject()) {
                return rpc::Ready(InvalidParams(QStringLiteral(
                    "store.set takes {plugin, document}: the id of a loaded "
                    "plugin, \"layout\" or \"settings\", and a JSON "
                    "object")));
            }
            const QJsonObject changed{
                {QStringLiteral("plugin"), plugin},
                {QStringLiteral("document"), document},
            };
            if (!Server::EventFitsAFrame(CHANGED, changed)) {
                return rpc::Ready(InvalidParams(QStringLiteral(
                    "the document is too big to be announced in a frame")));
            }
            const auto saved =
                self->Store_.Save(plugin.toString(), document.toObject());
            if (!saved) {
                return rpc::Ready(StoreFailed(saved.error()));
            }
            self->Server_.Bus().Publish(CHANGED, changed);
            return rpc::Ready(QJsonValue());
        });
    Q_ASSERT(setting);
    // The same steps as store.set, on the document as it is now: the event
    // loop runs a handler to its end, so the load, the merge and the save
    // never interleave with another request, and a patch of one key keeps what
    // another writer put in the others.
    const bool patching = Server_.Register(
        QStringLiteral("store.patch"),
        [self, gone](const rpc::Call& call) -> QFuture<rpc::Result> {
            if (!self) {
                return gone();
            }
            const QJsonObject params = call.Params.toObject();
            const QJsonValue plugin = params.value(QStringLiteral("plugin"));
            const QJsonValue patch = params.value(QStringLiteral("patch"));
            if (!plugin.isString() || !self->HasDocument(plugin.toString()) ||
                !patch.isObject()) {
                return rpc::Ready(InvalidParams(QStringLiteral(
                    "store.patch takes {plugin, patch}: the id of a loaded "
                    "plugin, \"layout\" or \"settings\", and a JSON object "
                    "(RFC 7386 merge patch)")));
            }
            auto document = self->Store_.Load(plugin.toString());
            if (!document) {
                return rpc::Ready(StoreFailed(document.error()));
            }
            ApplyMergePatch(*document, patch.toObject());
            const QJsonObject changed{
                {QStringLiteral("plugin"), plugin},
                {QStringLiteral("document"), *document},
            };
            if (!Server::EventFitsAFrame(CHANGED, changed)) {
                return rpc::Ready(InvalidParams(QStringLiteral(
                    "the document is too big to be announced in a frame")));
            }
            const auto saved = self->Store_.Save(plugin.toString(), *document);
            if (!saved) {
                return rpc::Ready(StoreFailed(saved.error()));
            }
            self->Server_.Bus().Publish(CHANGED, changed);
            return rpc::Ready(QJsonValue());
        });
    Q_ASSERT(patching);
    Q_UNUSED(getting);
    Q_UNUSED(setting);
    Q_UNUSED(patching);
}

StoreService::~StoreService() = default;

bool StoreService::HasDocument(const QString& id) const {
    return Store::IsWindowDocumentId(id) || Plugins_.Find(id) != nullptr;
}

}  // namespace roost
