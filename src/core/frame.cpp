#include "core/frame.hpp"

#include <QtEndian>

namespace roost {

namespace {

constexpr qsizetype HEADER_SIZE = sizeof(std::uint32_t);

bool IsKnownKind(std::uint8_t kind) {
    return kind <= static_cast<std::uint8_t>(EFrameKind::Snapshot);
}

}  // namespace

std::optional<QByteArray> EncodeFrame(EFrameKind kind, QByteArrayView body) {
    if (body.size() >= static_cast<qsizetype>(MAX_FRAME_LENGTH)) {
        return std::nullopt;
    }
    const auto length = static_cast<std::uint32_t>(body.size() + 1);

    QByteArray frame(HEADER_SIZE, Qt::Uninitialized);
    qToBigEndian(length, frame.data());
    frame.append(static_cast<char>(kind));
    frame.append(body);
    return frame;
}

std::optional<QByteArray> EncodeChannelFrame(EFrameKind kind,
                                             std::uint32_t channel,
                                             QByteArrayView bytes) {
    if (bytes.size() > MAX_CHANNEL_BYTES) {
        return std::nullopt;
    }
    QByteArray body(sizeof(channel), Qt::Uninitialized);
    qToBigEndian(channel, body.data());
    body.append(bytes);
    return EncodeFrame(kind, body);
}

std::optional<ChannelBody> DecodeChannelBody(QByteArrayView body) {
    if (body.size() < static_cast<qsizetype>(sizeof(std::uint32_t))) {
        return std::nullopt;
    }
    return ChannelBody{
        .Channel = qFromBigEndian<std::uint32_t>(body.data()),
        .Bytes = body.sliced(sizeof(std::uint32_t)).toByteArray(),
    };
}

void FrameDecoder::Feed(QByteArrayView bytes) {
    if (Failed()) {
        return;
    }
    Buffer_.append(bytes);
}

std::optional<Frame> FrameDecoder::Next() {
    if (Failed() || Buffer_.size() - Offset_ < HEADER_SIZE) {
        return std::nullopt;
    }

    const char* header = Buffer_.constData() + Offset_;
    const auto length = qFromBigEndian<std::uint32_t>(header);
    if (length == 0) {
        Error_ = QStringLiteral("frame of length 0 has no kind byte");
        return std::nullopt;
    }
    if (length > MAX_FRAME_LENGTH) {
        Error_ = QStringLiteral("frame of %1 bytes exceeds the limit of %2")
                     .arg(length)
                     .arg(MAX_FRAME_LENGTH);
        return std::nullopt;
    }
    // The kind is checked as soon as it arrives, not once the whole body is
    // in: a garbage stream should fail on its first bytes.
    if (Buffer_.size() - Offset_ > HEADER_SIZE) {
        const auto kind = static_cast<std::uint8_t>(header[HEADER_SIZE]);
        if (!IsKnownKind(kind)) {
            Error_ = QStringLiteral("unknown frame kind %1").arg(kind);
            return std::nullopt;
        }
    }
    if (Buffer_.size() - Offset_ < HEADER_SIZE + length) {
        return std::nullopt;
    }

    Frame frame;
    frame.Kind = static_cast<EFrameKind>(header[HEADER_SIZE]);
    frame.Body = Buffer_.mid(Offset_ + HEADER_SIZE + 1, length - 1);
    Offset_ += HEADER_SIZE + length;
    Compact();
    return frame;
}

void FrameDecoder::Compact() {
    // Drop consumed bytes once they are most of the buffer, so a long-lived
    // connection neither grows without bound nor copies on every frame.
    if (Offset_ == Buffer_.size()) {
        Buffer_.clear();
        Offset_ = 0;
    } else if (Offset_ > Buffer_.size() / 2) {
        Buffer_.remove(0, Offset_);
        Offset_ = 0;
    }
}

}  // namespace roost
