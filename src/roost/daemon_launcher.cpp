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
    const QString log =
        QFileInfo(socketPath).absolutePath() + QStringLiteral("/roostd.log");
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    if (!appImage.isEmpty()) {
        return {appImage, {QStringLiteral("--roostd"), "--log", log}};
    }
    return {QCoreApplication::applicationDirPath() + QStringLiteral("/roostd"),
            {QStringLiteral("--log"), log}};
}

bool DaemonLauncher::StartDetached(const Command& command) {
    QProcess process;
    process.setProgram(command.Program);
    process.setArguments(command.Arguments);
    process.setWorkingDirectory(QDir::homePath());
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    QProcess::UnixProcessParameters parameters;
    parameters.flags = QProcess::UnixProcessFlag::CreateNewSession |
                       QProcess::UnixProcessFlag::CloseFileDescriptors |
                       QProcess::UnixProcessFlag::ResetSignalHandlers;
    process.setUnixProcessParameters(parameters);
    return process.startDetached();
}

DaemonLauncher::DaemonLauncher(api::Api& api, Command command,
                               std::chrono::milliseconds startTimeout,
                               StartFunction start, QObject* parent)
    : QObject(parent), Command_(std::move(command)), Start_(std::move(start)) {
    Timeout_.setSingleShot(true);
    Timeout_.setInterval(startTimeout);
    connect(&Timeout_, &QTimer::timeout, this, [this] { SetStarting(false); });
    connect(&api, &api::Api::NoListener, this, &DaemonLauncher::OnNoListener);
    connect(&api, &api::Api::Connected, this, [this] { SetStarting(false); });
    connect(&api, &api::Api::Refused, this, [this] { SetStarting(false); });
}

void DaemonLauncher::OnNoListener() {
    if (Starting_) {
        return;
    }
    if (!Start_(Command_)) {
        qWarning("roost: cannot start %s", qPrintable(Command_.Program));
        return;
    }
    SetStarting(true);
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

}  // namespace roost
