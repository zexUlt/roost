#pragma once

#include <QString>

namespace roost::notifications {

// One line on stderr, which roostd passes on to its own: what a user of the
// plugin needs to see, such as a bad field of the settings.
void Warn(const QString& message);

}  // namespace roost::notifications
