#include <QCommandLineParser>
#include <QCoreApplication>

#include <chrono>
#include <csignal>

#include "codex/backend/adapter.hpp"
#include "common/backend_process.hpp"

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("Lists saved Codex sessions for roostd.");
    const QCommandLineOption pollMs("poll-ms", "Delay between lists.", "ms");
    const QCommandLineOption timeoutMs(
        "timeout-ms", "Time allowed for each App Server reply.", "ms");
    parser.addOptions({pollMs, timeoutMs});
    parser.addHelpOption();
    parser.process(app);

    roost::codex::Adapter adapter(
        qEnvironmentVariable("ROOST_CODEX_EXECUTABLE", "codex"),
        roost::plugin::Milliseconds(parser.value(pollMs),
                                    std::chrono::seconds(30)),
        roost::plugin::Milliseconds(parser.value(timeoutMs),
                                    std::chrono::seconds(15)),
        roost::plugin::SendStdio);
    return roost::plugin::RunStdio(
        app, [&adapter](QByteArrayView line) { return adapter.Handle(line); },
        [&adapter] { adapter.Start(); });
}
