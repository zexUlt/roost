#include "api/api.hpp"

#include <QtLogging>

#include <algorithm>
#include <utility>

#include "api/thread_check.hpp"
#include "core/protocol.hpp"

namespace roost::api {

Api::Api(ApiOptions options, QObject* parent)
    : QObject(parent),
      Options_(options),
      Terminals_(Io_),
      Bus_(Io_),
      Commands_(Io_),
      Plugins_(Io_),
      Sessions_(Io_, Bus_),
      Store_(Io_, Bus_),
      Dispatch_(Bus_, Commands_, Plugins_, Sessions_, Store_),
      Delay_(options.FirstReconnectDelay) {
    RetryTimer_.setSingleShot(true);
    connect(&RetryTimer_, &QTimer::timeout, this, &Api::Attempt);
    connect(&Io_, &IoThread::Disconnected, this, &Api::OnDisconnected);
    // Prefetch opens the hidden sessions and then reopens the shown one, so
    // that the session the window shows is the most recent in roostd's pool
    // and the last to be evicted.
    connect(this, &Api::Connected, &Terminals_, &WarmTerminals::Prefetch);
}

void Api::Connect(const QString& socketPath) {
    if (!CheckThread(*this, "Api::Connect")) {
        return;
    }
    if (Started_ && !Refused_) {
        qWarning(
            "Api::Connect is called once; the connection is left as it is");
        return;
    }
    Started_ = true;
    Refused_ = false;
    Reported_ = false;
    Delay_ = Options_.FirstReconnectDelay;
    SocketPath_ = socketPath;
    Attempt();
}

void Api::Attempt() {
    Io_.Connect(SocketPath_).then(this, [this](const rpc::Result& hello) {
        OnHello(hello);
    });
}

void Api::OnHello(const rpc::Result& hello) {
    if (!hello) {
        // A lost connection is reported by IoThread::Disconnected, which
        // schedules the next attempt.
        if (hello.error().Code !=
            static_cast<int>(protocol::EErrorCode::ConnectionLost)) {
            Refused_ = true;
            RefusalTail_ = true;
            RetryTimer_.stop();
            emit Refused(hello.error());
        }
        return;
    }
    RetryTimer_.stop();
    Connected_ = true;
    Reported_ = false;
    Delay_ = Options_.FirstReconnectDelay;
    // In this order: the listeners ask first, so that an event is not missed
    // between a cache's answer and its subscription.
    Bus_.Resubscribe();
    Plugins_.Fetch();
    Sessions_.Start();
    Store_.Start();
    emit Connected();
}

void Api::OnDisconnected(const QString& reason) {
    Connected_ = false;
    if (!std::exchange(RefusalTail_, false) && !Refused_) {
        RetryTimer_.start(Delay_);
        Delay_ = std::min(Delay_ * 2, Options_.MaxReconnectDelay);
    }
    if (std::exchange(Reported_, true)) {
        qDebug("the attempt failed, the next is scheduled: %s",
               qUtf8Printable(reason));
        return;
    }
    emit Disconnected(reason);
}

bool Api::IsConnected() const {
    return CheckThread(*this, "Api::IsConnected") && Connected_;
}

}  // namespace roost::api
