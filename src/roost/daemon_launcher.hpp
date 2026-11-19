#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtTypes>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

#include "api/api.hpp"
#include "roost/login_environment.hpp"

namespace roost {

// Starts roostd when the window finds nobody at the socket (design.md §4,
// "Запуск демона окном"). Plain C++, made from main() like Shell and
// SingleSession; GUI thread only.
//
// The trigger is Api::NoListener: no socket file, or a file nobody answers on.
// A server that answers and refuses (Api::Refused) is not one. A roostd that
// is up is not started over either, except when its accept backlog is full
// and the attempt is refused: the second one loses the socket lock and exits.
//
// One start is in flight at a time. It begins with the start of the process
// and ends when the Api connects or is refused, or after `StartTimeout`
// (roostd died while starting, or never came up): the Api's retry keeps
// reporting NoListener while the new roostd comes up, and none of those starts
// another. A NoListener after the end starts again, which is how a roostd that
// died later comes back.
//
// The environment of the new roostd is the one of LoginEnvironment (the login
// shell's when the window has no terminal). Given the settings for it, the
// launcher waits for the first read of it before its first start: a NoListener
// in the meantime starts nothing, "Starting" is true, and the start follows the
// read. Without settings the process is started with the environment as it is.
//
// Restart() is the button of the banner for a roostd of another version: it
// stops the old one by SIGTERM and starts a new one (below).
//
// A start that fails (the process does not start, or the timeout passes
// without a connection) leaves a Failure() text until roostd answers
// (Connected or Refused); later starts keep it, the window shows it in the
// banner.
class DaemonLauncher : public QObject {
    Q_OBJECT

public:
    struct Command {
        QString Program;
        QStringList Arguments;
        // Where the output of the process goes before it holds the socket lock
        // (after it roostd writes its own --log): appended to this file if its
        // directory exists, else /dev/null. Empty: /dev/null.
        QString StartLog;
        // The environment of the process; none: the one of this process.
        std::optional<QProcessEnvironment> Environment = std::nullopt;
    };

    // Starts the process; false, with `error` set, if it did not start. The
    // default detaches it (StartDetached). A test replaces it to count the
    // starts.
    using StartFunction = std::function<bool(const Command&, QString& error)>;

    static constexpr std::chrono::milliseconds DEFAULT_START_TIMEOUT{
        std::chrono::seconds(15)};
    // How long Restart() waits for the old roostd to let go of the socket.
    static constexpr std::chrono::milliseconds DEFAULT_STOP_TIMEOUT{
        std::chrono::seconds(5)};

    // The roostd of this installation: `roostd` next to the executable, or
    // `$APPIMAGE --roostd` inside an AppImage (APPIMAGE and APPDIR set, and
    // the executable inside APPDIR); `--log <dir of the socket>/roostd.log`
    // and StartLog `<dir of the socket>/roostd.start.log` either way.
    static Command DefaultCommand(const QString& socketPath);

    // DefaultCommand with the environment and the directory of the executable
    // given.
    static Command CommandFor(const QString& socketPath,
                              const QString& appImage, const QString& appDir,
                              const QString& executableDir);

    // Starts the process alone in its own session, with no terminal and no
    // inherited descriptors, in $HOME, so that it outlives the window. Its
    // standard output and error go to `command.StartLog`, else /dev/null. On
    // failure false and the reason in `error`.
    static bool StartDetached(const Command& command, QString& error);

    // `api` is listened to, not owned. `environment`: how to get the
    // environment of roostd (the window passes LoginEnvironment::Settings::
    // OfThisProcess()); none: `command` as it is.
    DaemonLauncher(
        api::Api& api, Command command,
        std::chrono::milliseconds startTimeout = DEFAULT_START_TIMEOUT,
        StartFunction start = &DaemonLauncher::StartDetached,
        std::optional<LoginEnvironment::Settings> environment = std::nullopt,
        std::chrono::milliseconds stopTimeout = DEFAULT_STOP_TIMEOUT,
        QObject* parent = nullptr);

    // Whether a start is in flight; the window shows "Starting roostd…".
    bool Starting() const { return Starting_; }

    // Why the last start failed, empty after a start, a connection or a
    // refusal; the window shows it in the banner.
    const QString& Failure() const { return Failure_; }

    // Whether Restart() is waiting for the old roostd to go.
    bool Restarting() const { return Restarting_; }

    // Replaces the roostd of `socketPath`, which refused the window (another
    // version); `peerPid` is the pid of the process that refused, as the
    // connection tells it (Api::RefusedPeerPid). Reads `<socketPath>.lock`
    // (QLockFile::getLockInfo) and sends SIGTERM only if the lock is held, has
    // the application name of roostd (protocol::DAEMON_APPLICATION_NAME) and
    // names `peerPid` (known, above 1, not this process); else Failure() says
    // why and nothing is killed. Then, without blocking, waits
    // up to the stop timeout for the lock to be released (roostd closes the
    // socket before it releases the lock); if it is not, Failure() says so
    // (no SIGKILL). When it is, starts roostd as OnNoListener does (same
    // in-flight guard and failure texts) and emits Restarted(): the Api has to
    // connect again. Does nothing while a restart waits.
    void Restart(const QString& socketPath, qint64 peerPid);

signals:
    void StartingChanged(bool starting);
    void FailureChanged();
    void RestartingChanged(bool restarting);
    // The new roostd is started (not yet answering).
    void Restarted();

private:
    void OnNoListener();
    void OnTimeout();
    // Starts roostd, or the read of the environment it waits for; false if
    // the process did not start.
    bool BeginStart();
    // Starts the process with the environment ready; sets Starting_ or
    // Failure_. False if it did not start.
    bool StartProcess();
    void PollStop();
    void FailRestart(const QString& reason);
    void SetRestarting(bool restarting);
    void SetStarting(bool starting);
    void SetFailure(QString failure);

    Command Command_;
    // None without settings; read at the first start.
    std::unique_ptr<LoginEnvironment> Login_;
    StartFunction Start_;
    QTimer Timeout_;
    QTimer StopPoll_;
    std::chrono::milliseconds StopTimeout_;
    QString LockPath_;
    qint64 StopPid_ = 0;
    QElapsedTimer StopClock_;
    bool Restarting_ = false;
    bool Starting_ = false;
    QString Failure_;
};

}  // namespace roost
