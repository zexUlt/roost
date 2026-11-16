#include "roostd/store_service.hpp"

#include <QDebug>
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
            if (!plugin.isString() ||
                self->Plugins_.Find(plugin.toString()) == nullptr) {
                return rpc::Ready(InvalidParams(QStringLiteral(
                    "store.get takes {plugin}: the id of a loaded plugin")));
            }
            const auto document = self->Store_.Load(plugin.toString());
            if (!document) {
                return rpc::Ready(StoreFailed(document.error()));
            }
            const QJsonObject answer{{QStringLiteral("document"), *document}};
            // Refused rather than closing the connection.
            if (!Server::AnswerFitsAFrame(answer)) {
                return rpc::Ready(StoreFailed(
                    QStringLiteral("the document of \"%1\" is too big to send "
                                   "in a frame")
                        .arg(plugin.toString())));
            }
            return rpc::Ready(answer);
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
            if (!plugin.isString() ||
                self->Plugins_.Find(plugin.toString()) == nullptr ||
                !document.isObject()) {
                return rpc::Ready(InvalidParams(
                    QStringLiteral("store.set takes {plugin, document}: the "
                                   "id of a loaded plugin and a JSON object")));
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
            if (!self->Server_.Bus().Publish(CHANGED, changed)) {
                qWarning().noquote()
                    << QStringLiteral(
                           "store.changed for \"%1\" was not announced: "
                           "the bus refused the publish")
                           .arg(plugin.toString());
            }
            return rpc::Ready(QJsonValue());
        });
    Q_ASSERT(setting);
    Q_UNUSED(getting);
    Q_UNUSED(setting);
}

StoreService::~StoreService() = default;

}  // namespace roost
