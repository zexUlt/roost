// claude-adapter: the backend of the claude plugin (design.md §7). Speaks
// JSON-RPC 2.0 with roostd over stdio, one compact message per line, and
// exits when its stdin closes.

#include <QCommandLineParser>
#include <QCoreApplication>

#include <chrono>
#include <csignal>

#include "claude/backend/adapter.hpp"
#include "common/backend_process.hpp"

int main(int argc, char** argv) {
    // A write to a closed stdout fails instead of killing the process.
    std::signal(SIGPIPE, SIG_IGN);
    QCoreApplication app(argc, argv);

    roost::claude::AdapterOptions options;
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Lists Claude Code background sessions for roostd.");
    const QCommandLineOption pollMs(
        "poll-ms", "Milliseconds between runs of claude agents.", "ms");
    const QCommandLineOption timeoutMs(
        "timeout-ms", "Milliseconds a run of claude agents may take.", "ms");
    const QCommandLineOption commandTimeoutMs(
        "command-timeout-ms",
        "Milliseconds any command (stop, rm, respawn, --bg) may take.", "ms");
    const QCommandLineOption newSessionTimeoutMs(
        "new-session-timeout-ms",
        "Milliseconds claude --bg may take; overrides --command-timeout-ms.",
        "ms");
    parser.addOptions(
        {pollMs, timeoutMs, commandTimeoutMs, newSessionTimeoutMs});
    parser.addHelpOption();
    parser.process(app);
    options.PollInterval =
        roost::plugin::Milliseconds(parser.value(pollMs), options.PollInterval);
    options.PollTimeout = roost::plugin::Milliseconds(parser.value(timeoutMs),
                                                      options.PollTimeout);
    // One value for every command: the defaults differ per command, a test
    // wants a short one for all.
    if (const std::chrono::milliseconds all = roost::plugin::Milliseconds(
            parser.value(commandTimeoutMs), std::chrono::milliseconds::zero());
        all > std::chrono::milliseconds::zero()) {
        options.Timeouts = {all, all, all, all, all};
    }
    options.Timeouts.NewSession = roost::plugin::Milliseconds(
        parser.value(newSessionTimeoutMs), options.Timeouts.NewSession);

    roost::claude::Adapter adapter(options, roost::plugin::SendStdio);

    return roost::plugin::RunStdio(
        app, [&adapter](QByteArrayView line) { return adapter.Handle(line); },
        [&adapter] { adapter.Start(); });
}
