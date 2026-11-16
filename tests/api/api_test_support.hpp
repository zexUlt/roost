#pragma once

#include <QEventLoop>
#include <QFuture>
#include <QFutureWatcher>
#include <QString>
#include <QTimer>

#include "core/rpc.hpp"

// What the tests of src/api/ share.
namespace roost::api_test {

constexpr int TIMEOUT_MS = 10000;

template <typename TValue>
bool WaitFor(const QFuture<TValue>& future, int timeoutMs = TIMEOUT_MS) {
    if (future.isFinished()) {
        return true;
    }
    QEventLoop loop;
    QFutureWatcher<TValue> watcher;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop,
                     &QEventLoop::quit);
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    watcher.setFuture(future);
    loop.exec();
    return future.isFinished();
}

// The error to show when a result is missing. QVERIFY2 computes its message
// before the check, so it must not call error() on a value.
inline QString Why(const rpc::Result& result) {
    return result ? QString() : result.error().Message;
}

}  // namespace roost::api_test
