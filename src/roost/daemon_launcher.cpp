#include "roost/daemon_launcher.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QtLogging>

#include <utility>

#include "core/protocol.hpp"

namespace roost {

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
    QObject* parent)
    : QObject(parent), Command_(std::move(command)), Start_(std::move(start)) {
    if (environment) {
        Login_ = std::make_unique<LoginEnvironment>(std::move(*environment));
        connect(Login_.get(), &LoginEnvironment::Finished, this, [this] {
            // Nobody has answered meanwhile (a connection ends the start).
            if (Starting_) {
                StartRoostd();
            }
        });
    }
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
    if (Starting_) {
        return;
    }
    if (Login_ != nullptr && !Login_->Ready()) {
        // The first start waits for the environment: Finished starts it (at
        // once, from here, if there is nothing to wait for). The timeout of
        // the start does not run before that (OnTimeout) and begins with it.
        SetStarting(true);
        Login_->Start();
        return;
    }
    StartRoostd();
}

void DaemonLauncher::StartRoostd() {
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
        return;
    }
    // A failure stays until roostd answers: the retries keep starting it, and
    // the banner keeps saying why the previous start failed.
    SetStarting(true);
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
