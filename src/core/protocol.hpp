#pragma once

#include <QString>

#include "core/rpc.hpp"

// What the window and roostd agree on over the socket (design.md §4) beyond
// the frames and JSON-RPC itself: the handshake, the error codes of the
// server and where the socket lives.
namespace roost::protocol {

// Bumped on any change a peer of the old version would get wrong.
inline constexpr int VERSION = 1;

// Errors of roostd and its client, in the range JSON-RPC leaves to servers.
enum class EErrorCode : int {
    HandshakeRequired = -32000,  // a method other than hello before hello
    VersionMismatch = -32001,    // hello with another protocol or library
    ConnectionLost = -32002,     // client side: the socket went away
};

rpc::Error MakeError(EErrorCode code, QString message,
                     QJsonValue data = QJsonValue::Undefined);

// The socket of roostd: ROOST_SOCKET if set, else roost/roostd.sock under
// $XDG_RUNTIME_DIR, else roost-<uid>/roostd.sock under the temporary
// directory ($TMPDIR on macOS, which is already per user).
QString SocketPath();

}  // namespace roost::protocol
