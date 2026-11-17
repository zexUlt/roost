#pragma once

#include <QString>

#include "terminal/io_thread.hpp"

namespace roost::testing {

// How many channels roostd still had to the session when it ended, apart from
// the one this opens itself; -1 if the check could not be made. A channel that
// should have been closed counts here, since roostd sends session.closed to
// each channel it has. The session ends through a direct channel, and a
// round-trip request after its session.closed makes sure the others have
// arrived: roostd answers in the order of the socket. The session must be one
// that fake_tui attaches to (it ends on "x"). Spins the event loop.
int ChannelsLeft(IoThread& io, const QString& agent, const QString& id,
                 int timeoutMs);

}  // namespace roost::testing
