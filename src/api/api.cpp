#include "api/api.hpp"

#include <QtLogging>

#include <utility>

#include "api/thread_check.hpp"
#include "core/protocol.hpp"

namespace roost::api {

Api::Api(QObject* parent)
    : QObject(parent),
      Bus_(Io_),
      Commands_(Io_),
      Plugins_(Io_),
      Dispatch_(Io_, Bus_, Commands_, Plugins_) {
    connect(&Io_, &IoThread::Disconnected, this, [this](const QString& reason) {
        Connected_ = false;
        emit Disconnected(reason);
    });
}

void Api::Connect(const QString& socketPath) {
    if (!CheckThread(*this, "Api::Connect")) {
        return;
    }
    if (std::exchange(Started_, true)) {
        qWarning(
            "Api::Connect is called once; the connection is left as it is");
        return;
    }
    Io_.Connect(socketPath).then(this, [this](const rpc::Result& hello) {
        if (!hello) {
            // A lost connection is reported by IoThread::Disconnected.
            if (hello.error().Code !=
                static_cast<int>(protocol::EErrorCode::ConnectionLost)) {
                emit Refused(hello.error());
            }
            return;
        }
        Connected_ = true;
        Plugins_.Fetch();
        emit Connected();
    });
}

bool Api::IsConnected() const {
    return CheckThread(*this, "Api::IsConnected") && Connected_;
}

}  // namespace roost::api
