#include "api/store_service.hpp"

#include <QJsonObject>

#include <utility>

#include "api/thread_check.hpp"

namespace roost::api {

namespace {

const QString PLUGIN = QStringLiteral("plugin");
const QString DOCUMENT = QStringLiteral("document");

}  // namespace

StoreService::StoreService(IoThread& io, BusService& bus, QObject* parent)
    : QObject(parent), Io_(io), Bus_(bus) {
    connect(&Io_, &IoThread::Disconnected, this, &StoreService::OnDisconnected);
}

void StoreService::Start() {
    if (CheckThread(*this, "StoreService::Start")) {
        Started_ = true;
    }
}

QFuture<rpc::Result> StoreService::Get(const QString& plugin) {
    if (!CheckThread(*this, "StoreService::Get")) {
        return rpc::Ready(std::unexpected(WrongThread()));
    }
    const auto warm = Cache_.constFind(plugin);
    if (warm != Cache_.cend()) {
        return rpc::Ready(QJsonObject{{DOCUMENT, *warm}});
    }
    // Decided now: an answer is kept only if the subscription was asked for
    // before the request, on the same socket, so that roostd hands out the
    // document and the events that follow it in this order.
    const bool tracked = Started_;
    if (tracked) {
        Subscribe();
    }
    return Io_
        .Request(QStringLiteral("store.get"), QJsonObject{{PLUGIN, plugin}})
        .then(this, [this, plugin, tracked,
                     epoch = Epoch_](rpc::Result answer) {
            if (answer && tracked && epoch == Epoch_) {
                const QJsonValue document = answer->toObject().value(DOCUMENT);
                if (document.isObject()) {
                    Cache_.insert(plugin, document.toObject());
                }
            }
            return answer;
        });
}

QFuture<rpc::Result> StoreService::Set(const QString& plugin,
                                       const QJsonValue& document) {
    if (!CheckThread(*this, "StoreService::Set")) {
        return rpc::Ready(std::unexpected(WrongThread()));
    }
    return Io_.Request(QStringLiteral("store.set"),
                       QJsonObject{{PLUGIN, plugin}, {DOCUMENT, document}});
}

bool StoreService::IsWarm(const QString& plugin) const {
    return CheckThread(*this, "StoreService::IsWarm") &&
           Cache_.contains(plugin);
}

void StoreService::Subscribe() {
    if (Subscription_ != 0) {
        return;
    }
    const auto subscription = Bus_.Subscribe(
        QStringLiteral("store.changed"), this,
        [this](const QString& /*name*/, const QJsonValue& payload) {
            OnChanged(payload);
        });
    if (subscription) {
        Subscription_ = *subscription;
    }
}

void StoreService::OnChanged(const QJsonValue& payload) {
    const QJsonObject object = payload.toObject();
    const QString plugin = object.value(PLUGIN).toString();
    const QJsonValue document = object.value(DOCUMENT);
    // A plugin nobody has read has nothing to keep fresh.
    const auto warm = Cache_.find(plugin);
    if (warm == Cache_.end() || !document.isObject()) {
        return;
    }
    *warm = document.toObject();
    emit Changed(plugin, *warm);
}

void StoreService::OnDisconnected() {
    ++Epoch_;
    Started_ = false;
    Bus_.Unsubscribe(std::exchange(Subscription_, 0));
    Cache_.clear();
}

}  // namespace roost::api
