#pragma once

#include <QIcon>

namespace roost {

// The icon of the window: PNGs of 16 to 512 pixels compiled into the
// executable, the same files that make the hicolor theme of the Linux package
// (packaging/linux/icons). PNG, not SVG, so that the window needs no image
// plugin for it.
QIcon WindowIcon();

// Sets WindowIcon() as the icon of every window of the application: under X11
// it is _NET_WM_ICON. Call after the QGuiApplication is made.
void InstallWindowIcon();

}  // namespace roost
