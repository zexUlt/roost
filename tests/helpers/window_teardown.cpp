#include "helpers/window_teardown.hpp"

#include <QCoreApplication>
#include <QWindow>

namespace roost::testing {

void QuiesceWindow(QObject* root) {
    auto* window = qobject_cast<QWindow*>(root);
    if (window == nullptr) {
        return;
    }
    window->hide();
    QCoreApplication::removePostedEvents(window);
}

void QuiesceWindows(const QObjectList& roots) {
    for (QObject* root : roots) {
        QuiesceWindow(root);
    }
}

}  // namespace roost::testing
