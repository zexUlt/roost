#pragma once

namespace roost {

// The Qt Quick Controls style of the window: Fusion, on every OS, so that the
// palette of the window (Main.qml, from Roost.theme) paints the controls the
// same way. The native style of macOS draws the controls by the system, not by
// a palette. Called first in main(), before any QML that imports
// QtQuick.Controls is loaded: the style cannot be changed after that.
void SelectControlsStyle();

// True when the style in effect is the one SelectControlsStyle() asked for. The
// smoke modes of roost exit with 1 when it is not, so that a main() without the
// call fails the smoke tests and the package checks.
bool ControlsStyleIsSelected();

}  // namespace roost
