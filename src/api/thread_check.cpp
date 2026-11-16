#include "api/thread_check.hpp"

#include <QByteArray>
#include <QThread>
#include <QtLogging>

#include <atomic>

namespace roost::api {

namespace {

void Fatal(const char* message) { qFatal("%s", message); }

std::atomic<CheckHandler>& Handler() {
    static std::atomic<CheckHandler> handler{&Fatal};
    return handler;
}

}  // namespace

CheckHandler SetCheckHandler(CheckHandler handler) {
    return Handler().exchange(handler != nullptr ? handler : &Fatal);
}

bool CheckThread(const QObject& owner, const char* method) {
#ifndef NDEBUG
    if (!owner.thread()->isCurrentThread()) {
        Handler().load()(
            QByteArray("roost::api: " + QByteArray(method) +
                       " was called from a thread other than the GUI thread")
                .constData());
        return false;
    }
#else
    Q_UNUSED(owner)
    Q_UNUSED(method)
#endif
    return true;
}

rpc::Error WrongThread() {
    return rpc::Error::FromCode(
        rpc::EErrorCode::InternalError,
        QStringLiteral("called from a thread other than the GUI thread"));
}

}  // namespace roost::api
