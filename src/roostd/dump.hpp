#pragma once

#include <QString>

#include "core/frame.hpp"

namespace roost {

// One frame as a line of text: its kind, then its body. A JSON-RPC body is
// printed as compact JSON; a channel frame as its channel, its size and its
// first bytes, escaped.
QString DescribeFrame(const Frame& frame);

// roostd --dump: connects to the roostd at the socket path, greets it,
// subscribes to every event and prints each frame it gets, one per line on
// stdout, until roostd closes the connection. Returns the exit code.
int Dump(const QString& socketPath);

}  // namespace roost
