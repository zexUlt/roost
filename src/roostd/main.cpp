#include <QCommandLineParser>
#include <QCoreApplication>
#include <QSocketNotifier>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>

#include "core/log_file.hpp"
#include "core/protocol.hpp"
#include "core/store.hpp"
#include "core/version.hpp"
#include "roostd/dump.hpp"
#include "roostd/manifest.hpp"
#include "roostd/plugins.hpp"
#include "roostd/server.hpp"
#include "roostd/session_channels.hpp"
#include "roostd/store_service.hpp"

namespace {

// The signal handler writes a byte here; the event loop reads it.
int STOP_PIPE[2] = {-1, -1};

void RequestStop(int /*signal*/) {
    const int savedErrno = errno;
    const char byte = 0;
    // The write end does not block: a full pipe already holds a stop request.
    [[maybe_unused]] const ssize_t written = ::write(STOP_PIPE[1], &byte, 1);
    errno = savedErrno;
}

// SIGTERM and SIGINT end the event loop, so the socket, its lock and the
// backends are cleaned up on the way out.
bool StopOnSignals(QCoreApplication& app) {
    if (::pipe(STOP_PIPE) != 0) {
        return false;
    }
    for (const int fd : STOP_PIPE) {
        // Child processes do not inherit it.
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
            return false;
        }
    }
    if (::fcntl(STOP_PIPE[1], F_SETFL, O_NONBLOCK) != 0) {
        return false;
    }
    struct sigaction action{};
    action.sa_handler = RequestStop;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (::sigaction(SIGTERM, &action, nullptr) != 0 ||
        ::sigaction(SIGINT, &action, nullptr) != 0) {
        return false;
    }
    auto* notifier =
        new QSocketNotifier(STOP_PIPE[0], QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated, &app,
                     &QCoreApplication::quit);
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    roost::OpenClosedStdio();
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("roostd"));
    QCoreApplication::setApplicationVersion(
        QString::fromStdString(roost::VersionString()));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("roost daemon: agent sessions, PTYs, plugin backends"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption dump(
        QStringLiteral("dump"),
        QStringLiteral("Connect to the running roostd and print every frame "
                       "it sends: answers and all events."));
    parser.addOption(dump);
    const QCommandLineOption log(
        QStringLiteral("log"),
        QStringLiteral("Write stdout and stderr, backends' too, to <file>, "
                       "keeping the previous one as <file>.1. Opened once "
                       "roostd holds the socket lock."),
        QStringLiteral("file"));
    parser.addOption(log);
    parser.process(app);

    if (parser.isSet(dump)) {
        return roost::Dump(roost::protocol::SocketPath());
    }

    const auto config = roost::ConfigDirectory();
    if (!config) {
        qCritical("roostd: %s", qUtf8Printable(config.error()));
        return 1;
    }
    if (!StopOnSignals(app)) {
        qCritical("roostd: cannot handle signals: %s", strerror(errno));
        return 1;
    }

    roost::Server server;
    const QString socketPath = roost::protocol::SocketPath();
    if (auto listening = server.Listen(socketPath); !listening) {
        qCritical("roostd: %s", qUtf8Printable(listening.error()));
        return 1;
    }
    // Only now: a roostd that lost the lock leaves the log of the winner alone.
    if (parser.isSet(log)) {
        if (auto redirected = roost::RedirectToLog(parser.value(log));
            !redirected) {
            qCritical("roostd: %s", qUtf8Printable(redirected.error()));
            return 1;
        }
    }
    qInfo("roostd: listening on %s", qUtf8Printable(socketPath));

    // Gone before the server.
    roost::PluginHost plugins(server);
    plugins.Load(roost::PluginSearchPath(
        *config, roost::BundledPluginsDirectory(
                     QCoreApplication::applicationDirPath())));
    for (const QString& problem : plugins.Registry().Problems()) {
        qWarning("roostd: %s", qUtf8Printable(problem));
    }
    qInfo("roostd: %zu plugins loaded", plugins.Registry().Plugins().size());

    // Gone before the plugins, like the channels.
    roost::StoreService store(roost::Store(*config), server,
                              plugins.Registry());

    // Gone before the plugins.
    roost::SessionChannels sessions(server, plugins);

    return QCoreApplication::exec();
}
