// claude-adapter: the backend of the claude plugin (design.md §7). Speaks
// JSON-RPC 2.0 with roostd over stdio, one compact message per line, and
// exits when its stdin closes.

#include <QByteArray>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QSocketNotifier>

#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>

#include "claude/backend/adapter.hpp"

namespace {

// roostd sends nothing near this long; a longer line means a broken peer.
constexpr qsizetype MAX_LINE_LENGTH = 16LL * 1024 * 1024;

// False if roostd is gone.
bool WriteLine(QByteArray message) {
    message.append('\n');
    qsizetype written = 0;
    while (written < message.size()) {
        const ssize_t n =
            ::write(STDOUT_FILENO, message.constData() + written,
                    static_cast<size_t>(message.size() - written));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += n;
    }
    return true;
}

void Send(const QByteArray& message) {
    if (!WriteLine(message)) {
        QCoreApplication::exit(1);
    }
}

std::chrono::milliseconds Milliseconds(const QString& text,
                                       std::chrono::milliseconds fallback) {
    bool ok = false;
    const qint64 value = text.toLongLong(&ok);
    return ok && value > 0 ? std::chrono::milliseconds(value) : fallback;
}

}  // namespace

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
        "Milliseconds any command (stop, rm, respawn) may take.", "ms");
    parser.addOptions({pollMs, timeoutMs, commandTimeoutMs});
    parser.addHelpOption();
    parser.process(app);
    options.PollInterval =
        Milliseconds(parser.value(pollMs), options.PollInterval);
    options.PollTimeout =
        Milliseconds(parser.value(timeoutMs), options.PollTimeout);
    // One value for every command: the defaults differ per command, a test
    // wants a short one for all.
    if (const std::chrono::milliseconds all = Milliseconds(
            parser.value(commandTimeoutMs), std::chrono::milliseconds::zero());
        all > std::chrono::milliseconds::zero()) {
        options.Timeouts = {all, all, all, all};
    }

    roost::claude::Adapter adapter(options, Send);

    QByteArray buffer;
    QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&input, &QSocketNotifier::activated, &app, [&] {
        std::array<char, 65536> chunk{};
        const ssize_t n = ::read(STDIN_FILENO, chunk.data(), chunk.size());
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            return;
        }
        if (n <= 0) {
            // roostd closed our stdin: time to go.
            input.setEnabled(false);
            QCoreApplication::quit();
            return;
        }
        buffer.append(chunk.data(), n);
        qsizetype start = 0;
        for (qsizetype end = buffer.indexOf('\n', start); end >= 0;
             end = buffer.indexOf('\n', start)) {
            const QByteArray line = buffer.sliced(start, end - start);
            start = end + 1;
            if (line.trimmed().isEmpty()) {
                continue;
            }
            adapter.Handle(line).then(QtFuture::Launch::Sync,
                                      [](const QByteArray& response) {
                                          if (!response.isEmpty()) {
                                              Send(response);
                                          }
                                      });
        }
        buffer.remove(0, start);
        if (buffer.size() > MAX_LINE_LENGTH) {
            std::fprintf(stderr,
                         "claude-adapter: roostd sent a line longer than "
                         "%lld bytes\n",
                         static_cast<long long>(MAX_LINE_LENGTH));
            input.setEnabled(false);
            QCoreApplication::exit(1);
        }
    });

    adapter.Start();
    return QCoreApplication::exec();
}
