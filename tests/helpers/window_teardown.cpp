#include "helpers/window_teardown.hpp"

#include <QCoreApplication>
#include <QThread>
#include <QWindow>

namespace roost::testing {

namespace {

// Long enough for a render thread woken by the hide, slowed down by a
// sanitizer and two other tests, to go back to waiting for events.
constexpr unsigned long RENDER_THREAD_SETTLE_MS = 50;

}  // namespace

void QuiesceWindow(QObject* root) {
    auto* window = qobject_cast<QWindow*>(root);
    if (window == nullptr) {
        return;
    }
    window->hide();
    QCoreApplication::removePostedEvents(window);
    // The hide releases the resources of the window, which wakes the render
    // thread. A destroy request that thread takes while awake stops it from
    // rendering but leaves it waiting for an event nobody sends, and
    // ~QQuickWindow waits for it forever (Qt 6.12,
    // QSGSoftwareRenderThread: WM_TryRelease ends the wait only of a thread
    // that sleeps). Destroyed while that thread sleeps, the window is safe.
    QThread::msleep(RENDER_THREAD_SETTLE_MS);
}

void QuiesceWindows(const QObjectList& roots) {
    for (QObject* root : roots) {
        QuiesceWindow(root);
    }
}

}  // namespace roost::testing
