#pragma once

#include <QByteArray>
#include <QString>

#include <expected>
#include <optional>

namespace roost {

// Replacing the running AppImage with a verified package
// (design/17-updates.md). POSIX file operations, no GUI; a failure is one text
// for the user and leaves the AppImage as it was.

// The path of the AppImage this process runs from, or nothing. `appImage` is
// $APPIMAGE (a file), `appDir` is $APPDIR, `executableDir` is the directory of
// this executable: as in DaemonLauncher, the executable has to be inside the
// AppDir of the running AppImage, so an APPIMAGE of another app's environment
// does not count. An extracted AppDir (no APPIMAGE) and a `cmake --install`
// tree give nothing.
std::optional<QString> AppImagePath(const QString& appImage,
                                    const QString& appDir,
                                    const QString& executableDir);

// The same for this process: its environment and the application directory
// (needs a QCoreApplication).
std::optional<QString> RunningAppImagePath();

// Replaces the AppImage at `appImage` with the package at `package`, whose
// SHA-256 is checked against `digest` (hex) again right now: the file sat in
// the cache. The package is copied to a temporary file in the directory of the
// AppImage (one filesystem), synced, given the mode of the AppImage plus
// 0755, and renamed over it: the AppImage is never written in place, the
// running process maps it. A symlink is followed: the file it points to is
// replaced and the link stays (a link in ~/bin must keep working). Any
// failure removes the temporary file and leaves the AppImage untouched.
std::expected<void, QString> ReplaceAppImage(const QString& appImage,
                                             const QString& package,
                                             const QByteArray& digest);

}  // namespace roost
