// notifications-backend: the backend of the notifications plugin (M8). Runs the
// commands of the user when a session needs a human, is done or failed; the
// contract (settings, environment, stdin, results) is described at Notifier,
// notifier.hpp. Speaks JSON-RPC 2.0 with roostd over stdio, one compact message
// per line, and exits when its stdin closes.
//
// ROOST_NOTIFICATIONS_TEST_TIMEOUT_MS replaces the 10 s a command may run; for
// tests only.

#include <QCoreApplication>

#include <csignal>

#include "common/backend_process.hpp"
#include "notifications/backend/notifier.hpp"

int main(int argc, char** argv) {
    // A write to a closed stdout fails instead of killing the process.
    std::signal(SIGPIPE, SIG_IGN);
    QCoreApplication app(argc, argv);

    roost::notifications::NotifierOptions options;
    options.Runner.Timeout = roost::plugin::Milliseconds(
        qEnvironmentVariable("ROOST_NOTIFICATIONS_TEST_TIMEOUT_MS"),
        options.Runner.Timeout);
    roost::notifications::Notifier notifier(options, roost::plugin::SendStdio);

    return roost::plugin::RunStdio(
        app, [&notifier](QByteArrayView line) { return notifier.Handle(line); },
        [&notifier] { notifier.Start(); });
}
