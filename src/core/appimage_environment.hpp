#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <optional>

namespace roost {

// What an AppImage puts into the environment of what runs inside it, and what
// the window takes out again before it starts something: roostd, the login
// shell (LoginEnvironment), the new window after an update (relaunch). Pure
// functions of their arguments; no Qt event loop, no GUI.

// Variables of the bundle itself (the runtime and AppRun): always dropped.
// They name the mount of this run, which is gone when this window quits.
const QStringList& AppImageBundleVariables();

// Variables AppRun and its hooks set for the Qt of the AppDir, and the user's
// own copies would not be: dropped only inside an AppImage.
const QStringList& AppImageLibraryVariables();

// Whether the process runs inside an AppImage or an extracted AppDir run
// through AppRun (which sets APPDIR but no APPIMAGE): `appDir` set and the
// executable directory inside it. (An APPDIR of another app's environment does
// not count.)
bool InsideAppImage(const QString& appDir, const QString& executableDir);

// `environment` without the variables of the bundle: AppImageBundleVariables
// always, AppImageLibraryVariables when `inAppImage`.
QProcessEnvironment StripAppImageEnvironment(QProcessEnvironment environment,
                                             bool inAppImage);

// Whether `directory` is the root of a mount: its device differs from that of
// its parent. An AppImage run through the runtime has its APPDIR mounted; one
// run with --appimage-extract-and-run has it extracted to a plain directory.
// Empty when the directory cannot be examined.
std::optional<bool> IsMountPoint(const QString& directory);

// The environment of the new version that the old window starts after an
// update: the environment of this window as it is inside an AppImage
// (StripAppImageEnvironment, inAppImage), i.e. what a start from the desktop
// would have. LD_LIBRARY_PATH is dropped as LoginEnvironment drops it (AppRun
// does not save the user's value to restore), PATH is kept as it is (AppRun
// does not change it).
//
// A window that runs from an extracted AppImage (APPDIR set and not a mount
// point: no FUSE here) starts the new one the same way, with
// APPIMAGE_EXTRACT_AND_RUN=1, or the new one fails to mount. The user's own
// APPIMAGE_EXTRACT_AND_RUN stays as it is. `appDirIsMountPoint` is
// IsMountPoint(APPDIR) of this process; unknown counts as a mount.
QProcessEnvironment RelaunchEnvironment(const QProcessEnvironment& environment,
                                        bool appDirIsMountPoint);

// The working directory of the new version: $OWD (where the user ran the
// AppImage from, set by the runtime) if it is a directory, else `home`.
QString RelaunchDirectory(const QProcessEnvironment& environment,
                          const QString& home);

}  // namespace roost
