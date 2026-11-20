#include "roost/controls_style.hpp"

#include <QLatin1StringView>
#include <QQuickStyle>

namespace roost {

namespace {
constexpr QLatin1StringView STYLE{"Fusion"};
}  // namespace

void SelectControlsStyle() {
    // setStyle, not QT_QUICK_CONTROLS_STYLE or a qtquickcontrols2.conf in the
    // resources: an environment variable of the user cannot replace it. The
    // tests that load Main.qml make the same call before their first engine.
    QQuickStyle::setStyle(QString(STYLE));
}

bool ControlsStyleIsSelected() { return QQuickStyle::name() == STYLE; }

}  // namespace roost
