#include "roost/daemon_launcher.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QtLogging>

#include <signal.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "core/protocol.hpp"

namespace roost {

namespace {
constexpr int STOP_POLL_MS = 50;
}  // namespace

DaemonLauncher::Command DaemonLauncher::DefaultCommand(
    const QString& socketPath) {
    return CommandFor(socketPath, qEnvironmentVariable("APPIMAGE"),
                      qEnvironmentVariable("APPDIR"),
                      QCoreApplication::applicationDirPath());
}

DaemonLauncher::Command DaemonLauncher::CommandFor(
    const QString& socketPath, const QString& appImage, const QString& appDir,
    const QString& executableDir) {
    const QString dir = QFileInfo(socketPath).absolutePath();
    const QString log = dir + QStringLiteral("/roostd.log");
    const QString startLog = dir + QStringLiteral("/roostd.start.log");
    // An APPIMAGE of another app's environment does not count: this
    // executable has to be inside the APPDIR of the running AppImage. An
    // extracted AppDir has no APPIMAGE: its sibling roostd is the one.
    if (!appImage.isEmpty() &&
        LoginEnvironment::InAppImage(appDir, executableDir)) {
        return {appImage, {QStringLiteral("--roostd"), "--log", log}, startLog};
    }
    return {executableDir + QStringLiteral("/roostd"),
            {QStringLiteral("--log"), log},
            startLog};
}

bool DaemonLauncher::StartDetached(const Command& command, QString& error) {
    QProcess process;
    process.setProgram(command.Program);
    process.setArguments(command.Arguments);
    process.setWorkingDirectory(QDir::homePath());
    if (command.Environment) {
        process.setProcessEnvironment(*command.Environment);
    }
    process.setStandardInputFile(QProcess::nullDevice());
    if (!command.StartLog.isEmpty() &&
        QFileInfo(command.StartLog).dir().exists()) {
        process.setStandardOutputFile(command.StartLog, QIODevice::Append);
        process.setStandardErrorFile(command.StartLog, QIODevice::Append);
    } else {
        process.setStandardOutputFile(QProcess::nullDevice());
        process.setStandardErrorFile(QProcess::nullDevice());
    }
    QProcess::UnixProcessParameters parameters;
    parameters.flags = QProcess::UnixProcessFlag::CreateNewSession |
                       QProcess::UnixProcessFlag::CloseFileDescriptors |
                       QProcess::UnixProcessFlag::ResetSignalHandlers;
    process.setUnixProcessParameters(parameters);
    if (!process.startDetached()) {
        error = process.errorString();
        return false;
    }
    return true;
}

DaemonLauncher::DaemonLauncher(
    api::Api& api, Command command, std::chrono::milliseconds startTimeout,
    StartFunction start, std::optional<LoginEnvironment::Settings> environment,
    std::chrono::milliseconds stopTimeout, QObject* parent)
    : QObject(parent),
      Command_(std::move(command)),
      Start_(std::move(start)),
      StopTimeout_(stopTimeout) {
    if (environment) {
        Login_ = std::make_unique<LoginEnvironment>(std::move(*environment));
        connect(Login_.get(), &LoginEnvironment::Finished, this, [this] {
            // Nobody has answered meanwhile (a connection ends the start).
            if (Starting_) {
                StartProcess();
            }
        });
    }
    StopPoll_.setInterval(STOP_POLL_MS);
    connect(&StopPoll_, &QTimer::timeout, this, &DaemonLauncher::PollStop);
    Timeout_.setSingleShot(true);
    Timeout_.setInterval(startTimeout);
    connect(&Timeout_, &QTimer::timeout, this, &DaemonLauncher::OnTimeout);
    connect(&api, &api::Api::NoListener, this, &DaemonLauncher::OnNoListener);
    const auto answered = [this] {
        SetFailure({});
        SetStarting(false);
    };
    connect(&api, &api::Api::Connected, this, answered);
    connect(&api, &api::Api::Refused, this, answered);
}

void DaemonLauncher::OnNoListener() {
    // A restart starts the new roostd itself, when the old one has gone.
    if (Starting_ || Restarting_) {
        return;
    }
    BeginStart();
}

bool DaemonLauncher::BeginStart() {
    if (Login_ != nullptr && !Login_->Ready()) {
        // The first start waits for the environment: Finished starts it (at
        // once, from here, if there is nothing to wait for). The timeout of
        // the start does not run before that (OnTimeout) and begins with it.
        SetStarting(true);
        Login_->Start();
        return true;
    }
    return StartProcess();
}

bool DaemonLauncher::StartProcess() {
    Command command = Command_;
    if (Login_ != nullptr) {
        command.Environment = Login_->Environment();
    }
    QString error;
    if (!Start_(command, error)) {
        qWarning("roost: cannot start %s: %s", qPrintable(Command_.Program),
                 qPrintable(error));
        SetFailure(QStringLiteral("Could not start roostd: %1").arg(error));
        SetStarting(false);
        return false;
    }
    // A failure stays until roostd answers: the retries keep starting it, and
    // the banner keeps saying why the previous start failed.
    SetStarting(true);
    return true;
}

void DaemonLauncher::Restart(const QString& socketPath, qint64 peerPid) {
    if (Restarting_) {
        return;
    }
    SetFailure({});
    LockPath_ = socketPath + QStringLiteral(".lock");
    QLockFile lock(LockPath_);
    lock.setStaleLockTime(0);
    qint64 pid = 0;
    QString host;
    QString application;
    // The refused connection tells who answered it; the lock only has to
    // agree. A pid from the lock alone could be another roostd's, or a reused
    // one.
    if (peerPid <= 1 || peerPid == QCoreApplication::applicationPid()) {
        FailRestart(QStringLiteral("the process that refused the window is "
                                   "not known (pid %1)")
                        .arg(peerPid));
        return;
    }
    if (!lock.getLockInfo(&pid, &host, &application)) {
        FailRestart(QStringLiteral("%1 names no process").arg(LockPath_));
        return;
    }
    // Only the process that refused is signalled, and only if the lock is
    // roostd's and names it. The host of the lock is not compared: a peer of
    // a local socket is on this host, and the host name changes with the
    // network.
    if (application != QLatin1String(protocol::DAEMON_APPLICATION_NAME)) {
        FailRestart(QStringLiteral("%1 is held by %2 (pid %3), not by roostd")
                        .arg(LockPath_, application, QString::number(pid)));
        return;
    }
    if (pid != peerPid) {
        FailRestart(QStringLiteral("%1 names pid %2, but the socket was "
                                   "answered by pid %3")
                        .arg(LockPath_)
                        .arg(pid)
                        .arg(peerPid));
        return;
    }
    // A lock that can be taken is a file left by a roostd that died, and its
    // pid may be anybody's by now.
    if (lock.tryLock(0)) {
        lock.unlock();
        FailRestart(QStringLiteral("%1 is not held by a running process")
                        .arg(LockPath_));
        return;
    }
    if (::kill(static_cast<pid_t>(pid), SIGTERM) != 0) {
        FailRestart(QStringLiteral("cannot signal pid %1: %2")
                        .arg(pid)
                        .arg(QString::fromLocal8Bit(std::strerror(errno))));
        return;
    }
    StopPid_ = pid;
    StopClock_.start();
    SetRestarting(true);
    StopPoll_.start();
}

void DaemonLauncher::PollStop() {
    QLockFile probe(LockPath_);
    probe.setStaleLockTime(0);
    if (probe.tryLock(0)) {
        // Released: the socket is closed. The probe lets go before the new
        // roostd takes the lock.
        probe.unlock();
        StopPoll_.stop();
        SetRestarting(false);
        if (BeginStart()) {
            emit Restarted();
        }
        return;
    }
    if (StopClock_.hasExpired(StopTimeout_.count())) {
        StopPoll_.stop();
        FailRestart(
            QStringLiteral("roostd (pid %1) did not exit in %2 s; stop "
                           "it yourself and press Retry")
                .arg(StopPid_)
                .arg(std::chrono::duration<double>(StopTimeout_).count(), 0,
                     'g', 3));
    }
}

void DaemonLauncher::FailRestart(const QString& reason) {
    SetFailure(QStringLiteral("Could not restart roostd: %1").arg(reason));
    SetRestarting(false);
}

void DaemonLauncher::SetRestarting(bool restarting) {
    if (std::exchange(Restarting_, restarting) != restarting) {
        emit RestartingChanged(restarting);
    }
}

void DaemonLauncher::OnTimeout() {
    if (Login_ != nullptr && !Login_->Ready()) {
        return;  // still reading the environment; the start has not begun
    }
    QString failure = QStringLiteral("roostd did not start");
    if (!Command_.StartLog.isEmpty() &&
        QFileInfo(Command_.StartLog).dir().exists()) {
        failure += QStringLiteral("; see %1").arg(Command_.StartLog);
    }
    // Before the end of the start, so that the banner changes once.
    SetFailure(std::move(failure));
    SetStarting(false);
}

void DaemonLauncher::SetStarting(bool starting) {
    if (starting) {
        Timeout_.start();
    } else {
        Timeout_.stop();
    }
    if (std::exchange(Starting_, starting) != starting) {
        emit StartingChanged(starting);
    }
}

void DaemonLauncher::SetFailure(QString failure) {
    if (Failure_ != failure) {
        Failure_ = std::move(failure);
        emit FailureChanged();
    }
}

}  // namespace roost
