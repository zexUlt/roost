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
    // executable has to be inside the APPDIR of the running AppImage.
    if (!appImage.isEmpty() && !appDir.isEmpty()) {
        const QString root = QDir::cleanPath(appDir);
        const QString executable = QDir::cleanPath(executableDir);
        if (executable == root ||
            executable.startsWith(root + QLatin1Char('/'))) {
            return {
                appImage, {QStringLiteral("--roostd"), "--log", log}, startLog};
        }
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

DaemonLauncher::DaemonLauncher(api::Api& api, Command command,
                               std::chrono::milliseconds startTimeout,
                               StartFunction start, QObject* parent)
    : QObject(parent), Command_(std::move(command)), Start_(std::move(start)) {
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
    QString error;
    if (!Start_(Command_, error)) {
        qWarning("roost: cannot start %s: %s", qPrintable(Command_.Program),
                 qPrintable(error));
        SetFailure(QStringLiteral("Could not start roostd: %1").arg(error));
        return;
    }
    // A failure stays until roostd answers: the retries keep starting it, and
    // the banner keeps saying why the previous start failed.
    SetStarting(true);
}

void DaemonLauncher::OnTimeout() {
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
