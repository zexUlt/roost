#pragma once

#include <QString>

#include <cstdio>

#include "core/frame.hpp"

namespace roost {

// One frame as a line of text: its kind, then its body. A JSON-RPC body is
// printed as it came, its whitespace turned into spaces, or escaped if it is
// not one JSON value; a channel frame as its channel, its size and its first
// bytes, escaped.
QString DescribeFrame(const Frame& frame);

// roostd --dump: connects to the roostd at the socket path, greets it,
// subscribes to every event and prints each frame it gets, one per line, to
// out. Returns the exit code: 0 once roostd closes the connection or the
// reader of out goes away, 1 if roostd cannot be reached, refuses the
// greeting or breaks the stream.
int Dump(const QString& socketPath, std::FILE* out = stdout);

}  // namespace roost
