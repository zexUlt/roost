#pragma once

#include <QObject>

namespace roost::testing {

// Makes a window safe to destroy: hides it and drops its posted events.
// A window of the software scene graph under the threaded render loop (the
// loop of the offscreen platform, and of the tsan-qt run) has a render thread
// that reads the window while it draws. Destroying the window under it is a
// data race on the vptr in ~QQuickWindowQmlImpl, and sometimes
// ~QQuickWindow waits forever for that thread
// (QSGSoftwareThreadedRenderLoop::windowDestroyed). Hiding the window waits
// for the render thread to stop; the posted events, update requests among
// them, go before they reach a window that is going away.
//
// Call it before the engine or the window is destroyed. `root` may be anything
// but a window: it is then left alone, so a test can pass the first root
// object of an engine without asking what it is.
void QuiesceWindow(QObject* root);

// Each of the roots, as `QQmlApplicationEngine::rootObjects()` lists them.
void QuiesceWindows(const QObjectList& roots);

}  // namespace roost::testing
