#pragma once

#include <ghostty/vt.h>

#include <QString>

namespace roost {

// Internal to core: public headers must not include this one, so that
// libghostty-vt stays out of the include paths of the other modules.
//
// "<what>: <reason>", for an error message about a failed library call.
QString Failed(const char* what, GhosttyResult result);

}  // namespace roost
