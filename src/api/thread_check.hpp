#pragma once

#include <QObject>

#include "core/rpc.hpp"

namespace roost::api {

// Called with a description when a debug-build check fails. Must not return
// normally unless a test replaced it; the checked method then does nothing
// and reports failure.
using CheckHandler = void (*)(const char* message);

// Replaces the handler and returns the previous one; null restores the
// default, qFatal. For tests: not thread-safe against running checks.
CheckHandler SetCheckHandler(CheckHandler handler);

// The services and the dispatcher are used from the GUI thread only
// (design.md §5). Every public method starts with this, with the object it
// belongs to: true if the current thread is the thread of `owner`. In a debug
// build a failed check calls the handler first; a release build does not check
// and returns true.
bool CheckThread(const QObject& owner, const char* method);

// What a method that failed its check reports, if the handler returned.
rpc::Error WrongThread();

}  // namespace roost::api
