#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <cstdint>
#include <optional>

namespace roost {

// The framing of the window–daemon socket (design.md §4):
//
//   u32 length | u8 kind | body
//
// The length is big-endian and counts the bytes after itself: the kind byte
// and the body. It is therefore at least 1.
enum class EFrameKind : std::uint8_t {
    Rpc = 0,       // JSON-RPC 2.0, UTF-8
    Output = 1,    // u32 channel, PTY bytes; daemon to window
    Input = 2,     // u32 channel, input bytes; window to daemon
    Snapshot = 3,  // u32 channel, terminal snapshot; daemon to window
};

// The largest length field: kind byte and body, without the four bytes of the
// length itself. More is a protocol error, not a big message: the peer is
// broken or hostile, and buffering it would let it take the daemon's memory.
inline constexpr std::uint32_t MAX_FRAME_LENGTH = 16U * 1024U * 1024U;

struct Frame {
    EFrameKind Kind = EFrameKind::Rpc;
    QByteArray Body;
};

// The bytes of one frame, ready to write, or nothing if the body does not fit
// MAX_FRAME_LENGTH together with the kind byte. The body is not looked into:
// the channel number of kinds 1-3 is the business of the layer above.
[[nodiscard]] std::optional<QByteArray> EncodeFrame(EFrameKind kind,
                                                    QByteArrayView body);

// Turns a byte stream, cut into pieces anywhere, back into frames. No I/O: the
// caller feeds what it read and takes frames out.
//
// An oversized length, a zero length or an unknown kind fails the decoder for
// good: after that the stream has no frame boundaries to trust, and the only
// sane thing is to drop the connection. Bytes are only checked by Next(), so
// Failed() is meaningful after it, not right after Feed().
class FrameDecoder {
public:
    void Feed(QByteArrayView bytes);

    // The next complete frame, or nothing if more bytes are needed or the
    // decoder has failed.
    std::optional<Frame> Next();

    bool Failed() const { return !Error_.isEmpty(); }
    const QString& Error() const { return Error_; }

    // Bytes held in memory, frames already taken included until the buffer is
    // compacted. Bounded on a healthy stream; stops growing after a failure.
    qsizetype Buffered() const { return Buffer_.size(); }

private:
    void Compact();

    QByteArray Buffer_;
    qsizetype Offset_ = 0;
    QString Error_;
};

}  // namespace roost
