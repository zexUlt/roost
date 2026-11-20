#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QFuture>
#include <QString>

#include <chrono>
#include <functional>

class QCoreApplication;
class QProcess;

namespace roost::plugin {

// The home directory a CLI child runs in, so that the CLI does not take the
// installed plugin package, where the backend starts, for a project. Empty
// when the home is not an existing directory: the child then keeps the
// backend's directory, as it would without this.
QString HomeDirectory();

// Stops a child started in its own session and the process group it leads.
void StopRun(QProcess& process);

std::chrono::milliseconds Milliseconds(const QString& text,
                                       std::chrono::milliseconds fallback);

// The line-delimited JSON-RPC stream to roostd. Backends ignore SIGPIPE so a
// failed write can exit the event loop and clean up their child processes.
using HandleStdio = std::function<QFuture<QByteArray>(QByteArrayView)>;
void SendStdio(const QByteArray& message);
int RunStdio(QCoreApplication& app, const HandleStdio& handle,
             const std::function<void()>& startAdapter);

}  // namespace roost::plugin
