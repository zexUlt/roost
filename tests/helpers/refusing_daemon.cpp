// A roostd of another version as a process, for the tests of the restart: it
// takes the lock `<socket>.lock` as roostd does (QLockFile, no stale time),
// listens on the socket and refuses every hello with VersionMismatch
// (RefusingServer). The lock carries the name of the executable, so the test
// runs it as a hard link or copy named `roostd` (the lock of a roostd) or
// otherwise.
//
//   refusing_daemon <socket> [--ignore-term] [--exit-delay <ms>]
//                   [--marker <file>] [--answer-version <roostd>]
//                   [--answer-no-version]
//
// SIGTERM creates the marker file. Without --ignore-term it then ends as
// roostd does: the socket closes, then the lock is released. With it SIGTERM
// changes nothing else, so that the test can tell that the signal came and
// that the process is still there. --exit-delay keeps the socket and the lock
// for that long after SIGTERM, as a roostd that is busy stopping does.
// --answer-version and --answer-no-version make it answer hello successfully
// with that `roostd` version, or with none: a roostd of another build. Prints
// "ready" when it listens.

#include <QCoreApplication>
#include <QLockFile>
#include <QSocketNotifier>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <fcntl.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <optional>

#include "helpers/refusing_server.hpp"

namespace {

int STOP_PIPE[2] = {-1, -1};
bool IGNORE_TERM = false;
const char* MARKER_PATH = nullptr;

// Async-signal-safe calls only.
void OnTerm(int /*signal*/) {
    if (MARKER_PATH != nullptr) {
        const int fd = ::open(MARKER_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            ::close(fd);
        }
    }
    if (!IGNORE_TERM) {
        const char byte = 1;
        [[maybe_unused]] const ssize_t written =
            ::write(STOP_PIPE[1], &byte, 1);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() < 2) {
        std::fputs("usage: refusing_daemon <socket> [options]\n", stderr);
        return 2;
    }
    QByteArray marker;
    int exitDelayMs = 0;
    std::optional<QString> answerVersion;
    bool answerNoVersion = false;
    for (qsizetype i = 2; i < arguments.size(); ++i) {
        if (arguments[i] == QLatin1String("--marker") &&
            i + 1 < arguments.size()) {
            marker = arguments[++i].toLocal8Bit();
        } else if (arguments[i] == QLatin1String("--exit-delay") &&
                   i + 1 < arguments.size()) {
            exitDelayMs = arguments[++i].toInt();
        } else if (arguments[i] == QLatin1String("--answer-version") &&
                   i + 1 < arguments.size()) {
            answerVersion = arguments[++i];
        } else if (arguments[i] == QLatin1String("--answer-no-version")) {
            answerNoVersion = true;
        } else if (arguments[i] == QLatin1String("--ignore-term")) {
            IGNORE_TERM = true;
        } else {
            std::fprintf(stderr, "refusing_daemon: unknown argument %s\n",
                         qPrintable(arguments[i]));
            return 2;
        }
    }
    const QString socket = arguments[1];
    QLockFile lock(socket + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        std::fputs("refusing_daemon: the socket lock is held\n", stderr);
        return 3;
    }
    // After the lock, as roostd: what is left at the path is a dead one's.
    QLocalServer::removeServer(socket);

    if (::pipe(STOP_PIPE) != 0) {
        return 4;
    }
    MARKER_PATH = marker.isEmpty() ? nullptr : marker.constData();
    struct sigaction action{};
    action.sa_handler = &OnTerm;
    sigemptyset(&action.sa_mask);
    ::sigaction(SIGTERM, &action, nullptr);
    QSocketNotifier notifier(STOP_PIPE[0], QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, &app, [&] {
        notifier.setEnabled(false);
        QTimer::singleShot(exitDelayMs, &app, &QCoreApplication::quit);
    });

    int code = 0;
    {
        // Closes the socket before the lock is released.
        roost::testing::RefusingServer server;
        if (answerVersion) {
            server.SetAnswerWithVersion(*answerVersion);
        } else if (answerNoVersion) {
            server.SetAnswerWithoutVersion();
        }
        if (!server.Listen(socket)) {
            std::fputs("refusing_daemon: cannot listen\n", stderr);
            code = 5;
        } else {
            std::fputs("ready\n", stdout);
            std::fflush(stdout);
            code = app.exec();
        }
    }
    return code;
}
