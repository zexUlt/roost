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
    HandshakeRequired = -32000,   // a method other than hello before hello
    VersionMismatch = -32001,     // hello with another protocol or library
    ConnectionLost = -32002,      // client side: the socket went away
    BackendUnavailable = -32003,  // the plugin's backend is not running
    StoreFailed = -32004,         // reading or writing a plugin's state failed
};

// The name of the roostd executable. QLockFile writes the name of the running
// executable into the lock of the socket (not
// QCoreApplication::applicationName), and the window sends SIGTERM to the pid
// in the lock only if it is this name.
inline constexpr const char* DAEMON_APPLICATION_NAME = "roostd";

// The key of the `data` of a ConnectionLost error that says nobody listens at
// the socket path (no file, or a file nobody answers on): `{"noListener":
// true}`. A server that answers and then refuses or drops the client is not
// that, and the window must not start a roostd for it.
inline constexpr const char* NO_LISTENER_KEY = "noListener";

// The key the client adds to the `data` of a refused hello: the pid of the
// process at the other end of the socket (`{"peerPid": 123}`), read from the
// connection by the client, not sent by roostd. Absent if the system does not
// tell it.
inline constexpr const char* PEER_PID_KEY = "peerPid";

rpc::Error MakeError(EErrorCode code, QString message,
                     QJsonValue data = QJsonValue::Undefined);

// The socket of roostd: ROOST_SOCKET if set, else roost/roostd.sock under
// $XDG_RUNTIME_DIR, else roost-<uid>/roostd.sock under the temporary
// directory ($TMPDIR on macOS, which is already per user).
QString SocketPath();

}  // namespace roost::protocol
