// The installer of the AppImage (design/17-updates.md): the running AppImage
// file is replaced by the downloaded one with a rename (core/appimage_install),
// and the new one is started detached with the environment of a start from the
// desktop. The old file is not kept: its inode lives on while this window runs
// from it, and the rename is atomic, so there is no state in between.

#include <QCoreApplication>
#include <QDir>
#include <QProcess>

#include "core/appimage_environment.hpp"
#include "core/appimage_install.hpp"
#include "qml_api/update_install.hpp"

namespace roost {

QString installTarget() { return RunningAppImagePath().value_or(QString()); }

std::expected<InstallDone, QString> installPackage(
    const InstallRequest& request) {
    // No version check of the file, unlike the macOS bundle: it is not run to
    // ask (design/17-updates.md, "Версия"). Request.Rename and Request.Trash do
    // not apply: ReplaceAppImage renames by itself, and there is no old file
    // to move away.
    if (const auto replaced =
            ReplaceAppImage(request.Target, request.Package, request.Digest);
        !replaced) {
        return std::unexpected(replaced.error());
    }
    // Nothing of the old version is left to point to: InstallDone::Old empty.
    return InstallDone{};
}

std::expected<void, QString> relaunch(const QString& target) {
    // This window runs from the mount of the old AppImage, which goes with the
    // window: the new one must not find it in APPDIR, QT_PLUGIN_PATH and the
    // like, and starts in the directory the user started this one from.
    const QProcessEnvironment current =
        QProcessEnvironment::systemEnvironment();
    QProcess process;
    process.setProgram(target);
    process.setProcessEnvironment(RelaunchEnvironment(current));
    process.setWorkingDirectory(RelaunchDirectory(current, QDir::homePath()));
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    // Its own session, as for roostd (DaemonLauncher): it outlives this window
    // and has no controlling terminal of ours.
    QProcess::UnixProcessParameters parameters;
    parameters.flags = QProcess::UnixProcessFlag::CreateNewSession |
                       QProcess::UnixProcessFlag::CloseFileDescriptors |
                       QProcess::UnixProcessFlag::ResetSignalHandlers;
    process.setUnixProcessParameters(parameters);
    if (!process.startDetached()) {
        return std::unexpected(QStringLiteral("cannot start %1: %2")
                                   .arg(target, process.errorString()));
    }
    return {};
}

}  // namespace roost
