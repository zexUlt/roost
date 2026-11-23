#pragma once

#include <QByteArray>
#include <QString>
#include <QtSystemDetection>

#include <expected>
#include <functional>

namespace roost {

// The installer of a downloaded and verified release package
// (design/17-updates.md): one function per platform, chosen at compile time
// like packageAssetName. macOS: update_install_macos.cpp; Linux:
// update_install_appimage.cpp; any other system:
// update_install_unsupported.cpp. Plain functions, no Qt event loop:
// installPackage() blocks and is for a worker thread.

// What to replace and with what.
struct InstallRequest {
    // The verified package in the cache of the downloads.
    QString Package;
    // Its line of the signed SHA256SUMS (hex): the package is checked against
    // it once more right before it is used.
    QByteArray Digest;
    // The version the package is offered as, "X.Y.Z": what the installed
    // thing must say it is.
    QString Version;
    // What is replaced: installTarget().
    QString Target;
    // For tests, which cannot make a disk fail on demand: the renames of the
    // swap. Returns the reason of a failure, empty on success. Empty function:
    // the system's rename.
    std::function<QString(const QString& from, const QString& to)> Rename;
    // For tests, which must not touch the user's Trash: moves the old version
    // away and returns where it went, empty on a failure. Empty function: the
    // system's Trash.
    std::function<QString(const QString& path)> Trash;
};

// What a successful install left behind.
struct InstallDone {
    // The replaced thing: in the Trash, or, if the Trash refused it, where it
    // was left (a warning is written). Empty where nothing of it is kept: the
    // AppImage is replaced by a rename.
    QString Old;
};

// What this run can replace in place: the path of the running bundle on macOS,
// of the running AppImage file on Linux (RunningAppImagePath).
// Empty when this run is not an installed copy (a build tree, an extracted
// tree, a `cmake --install` tree), when it is not where an update can replace
// it (macOS: App Translocation, a read-only volume such as the mounted .dmg),
// or this platform has no installer.
QString installTarget();

#ifdef Q_OS_MACOS
// The decision of installTarget() on macOS over what it looks at, so that a
// test needs no real translocation: `executableDirectory` is the directory of
// the running executable (QCoreApplication::applicationDirPath()), `readOnly`
// is whether the volume of the bundle is mounted read-only. The bundle's path,
// empty if the executable is not in <name>.app/Contents/MacOS, or the bundle
// is under /AppTranslocation/, or `readOnly`.
QString installTargetOf(const QString& executableDirectory, bool readOnly);

// Whether the volume `path` is on is mounted read-only (statfs MNT_RDONLY);
// false if that cannot be told.
bool onReadOnlyVolume(const QString& path);
#endif

// Replaces `request.Target` with the release in `request.Package`. A failure
// leaves the target as it was and says why, with the path.
std::expected<InstallDone, QString> installPackage(
    const InstallRequest& request);

// Starts the replaced `target` detached. Does not quit this process. On Linux
// with the environment of a start from the desktop, not of this window
// (RelaunchEnvironment); its output goes to roost.relaunch.log beside the other
// logs.
std::expected<void, QString> relaunch(const QString& target);

}  // namespace roost
