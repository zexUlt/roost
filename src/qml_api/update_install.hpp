#pragma once

#include <QByteArray>
#include <QString>

#include <expected>
#include <functional>

namespace roost {

// The installer of a downloaded and verified release package
// (design/17-updates.md): one function per platform, chosen at compile time
// like packageAssetName. macOS: update_install_macos.cpp; any other system:
// update_install_unsupported.cpp. The AppImage installer (#251) is a third file
// with the same functions. Plain functions, no Qt event loop: installPackage()
// blocks and is for a worker thread.

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
};

// What a successful install left behind.
struct InstallDone {
    // The replaced thing: in the Trash, or, if the Trash refused it, where it
    // was left (a warning is written).
    QString Old;
};

// What this run can replace in place: the path of the running bundle on macOS.
// Empty when this run is not an installed copy (a build tree, an extracted
// tree) or this platform has no installer.
QString installTarget();

// Replaces `request.Target` with the release in `request.Package`. A failure
// leaves the target as it was and says why, with the path.
std::expected<InstallDone, QString> installPackage(
    const InstallRequest& request);

// Starts the replaced `target` detached. Does not quit this process.
std::expected<void, QString> relaunch(const QString& target);

}  // namespace roost
