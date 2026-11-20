#include "common/backend_process.hpp"

#include <QCoreApplication>
#include <QProcess>
#include <QSocketNotifier>

#include <signal.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>

namespace roost::plugin {

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

}  // namespace

void StopRun(QProcess& process) {
    if (process.state() == QProcess::NotRunning) {
        return;
    }
    // A run that failed to start (chdir failed, say) reports it while the
    // state is still Starting, with no process: the pid is 0, and killpg(0)
    // is the group of the adapter itself. There is nothing to signal or to
    // wait for, and waitForFinished would block for its whole timeout.
    const auto group = static_cast<pid_t>(process.processId());
    if (group <= 0) {
        return;
    }
    ::killpg(group, SIGKILL);
    process.kill();
    process.waitForFinished();
    // A fork that raced with the first signal may have joined the group
    // after it was delivered; the group lives on while it has a member.
    ::killpg(group, SIGKILL);
}

std::chrono::milliseconds Milliseconds(const QString& text,
                                       std::chrono::milliseconds fallback) {
    bool ok = false;
    const qint64 value = text.toLongLong(&ok);
    return ok && value > 0 ? std::chrono::milliseconds(value) : fallback;
}

void SendStdio(const QByteArray& message) {
    if (!message.isEmpty() && !WriteLine(message)) {
        QCoreApplication::exit(1);
    }
}

int RunStdio(QCoreApplication& app, const HandleStdio& handle,
             const std::function<void()>& startAdapter) {
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
            if (end - start > MAX_LINE_LENGTH) {
                break;
            }
            const QByteArray line = buffer.sliced(start, end - start);
            start = end + 1;
            if (line.trimmed().isEmpty()) {
                continue;
            }
            handle(line).then(QtFuture::Launch::Sync,
                              [](const QByteArray& response) {
                                  if (!response.isEmpty()) {
                                      SendStdio(response);
                                  }
                              });
        }
        buffer.remove(0, start);
        if (buffer.size() > MAX_LINE_LENGTH) {
            std::fprintf(stderr,
                         "%s: roostd sent a line longer than %lld bytes\n",
                         qUtf8Printable(app.applicationName()),
                         static_cast<long long>(MAX_LINE_LENGTH));
            input.setEnabled(false);
            QCoreApplication::exit(1);
        }
    });

    startAdapter();
    return QCoreApplication::exec();
}

}  // namespace roost::plugin
