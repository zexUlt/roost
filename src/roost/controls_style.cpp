#include "roost/controls_style.hpp"

#include <QQuickStyle>

namespace roost {

void SelectControlsStyle() {
    // setStyle, not QT_QUICK_CONTROLS_STYLE or a qtquickcontrols2.conf in the
    // resources: the call is the same for the window and the tests that load
    // Main.qml, and an environment variable of the user cannot replace it.
    QQuickStyle::setStyle(QStringLiteral("Fusion"));
}

}  // namespace roost
