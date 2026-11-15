// The frame codec (design.md §4): a stream cut anywhere decodes to the same
// frames, and a malformed stream fails on its first bad bytes.

#include <QByteArray>
#include <QList>
#include <QRandomGenerator>
#include <QTest>
#include <QtEndian>

#include <cstdint>

#include "core/frame.hpp"

using roost::EFrameKind;
using roost::Frame;
using roost::FrameDecoder;

namespace {

QList<Frame> SampleFrames() {
    QByteArray big(70000, 'x');
    for (qsizetype i = 0; i < big.size(); ++i) {
        big[i] = static_cast<char>(i % 251);
    }
    return {
        {EFrameKind::Rpc, R"({"jsonrpc":"2.0","method":"hello","id":1})"},
        {EFrameKind::Output, QByteArray("\x00\x00\x00\x07\x1b[31mred", 12)},
        {EFrameKind::Input, QByteArray()},
        {EFrameKind::Snapshot, big},
        {EFrameKind::Rpc, QByteArray(1, '\0')},
    };
}

QByteArray EncodeAll(const QList<Frame>& frames) {
    QByteArray stream;
    for (const Frame& frame : frames) {
        stream += roost::EncodeFrame(frame.Kind, frame.Body);
    }
    return stream;
}

// Feeds the stream cut at the given offsets and collects every frame.
QList<Frame> DecodeInPieces(const QByteArray& stream,
                            const QList<qsizetype>& cuts) {
    FrameDecoder decoder;
    QList<Frame> frames;
    qsizetype start = 0;
    auto feed = [&](qsizetype end) {
        decoder.Feed(QByteArrayView(stream).sliced(start, end - start));
        start = end;
        while (auto frame = decoder.Next()) {
            frames.append(*frame);
        }
    };
    for (const qsizetype cut : cuts) {
        feed(cut);
    }
    feed(stream.size());
    return frames;
}

bool SameFrames(const QList<Frame>& a, const QList<Frame>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (a[i].Kind != b[i].Kind || a[i].Body != b[i].Body) {
            return false;
        }
    }
    return true;
}

QByteArray Header(std::uint32_t length, std::uint8_t kind) {
    QByteArray bytes(4, Qt::Uninitialized);
    qToBigEndian(length, bytes.data());
    bytes.append(static_cast<char>(kind));
    return bytes;
}

}  // namespace

class FrameTest : public QObject {
    Q_OBJECT

private slots:
    void EncodesLengthBigEndianAfterItself();
    void WholeStreamAtOnce();
    void EverySingleCut();
    void EveryPairOfCutsInTheSmallFrames();
    void ByteByByte();
    void RandomPieces();
    void ZeroLengthFails();
    void OversizedLengthFailsBeforeTheBody();
    void UnknownKindFailsBeforeTheBody();
    void FailureIsFinal();
};

void FrameTest::EncodesLengthBigEndianAfterItself() {
    QCOMPARE(roost::EncodeFrame(EFrameKind::Input, "ab"),
             QByteArray("\x00\x00\x00\x03\x02"
                        "ab",
                        7));
    QCOMPARE(roost::EncodeFrame(EFrameKind::Rpc, {}),
             QByteArray("\x00\x00\x00\x01\x00", 5));
}

void FrameTest::WholeStreamAtOnce() {
    const QList<Frame> frames = SampleFrames();
    QVERIFY(SameFrames(DecodeInPieces(EncodeAll(frames), {}), frames));
}

void FrameTest::EverySingleCut() {
    const QList<Frame> frames = SampleFrames();
    const QByteArray stream = EncodeAll(frames);
    for (qsizetype cut = 0; cut <= stream.size(); ++cut) {
        if (!SameFrames(DecodeInPieces(stream, {cut}), frames)) {
            QFAIL(qPrintable(QStringLiteral("cut at %1").arg(cut)));
        }
    }
}

// Every way to cut a stream into three pieces. Quadratic, so only over the
// frames that are small enough; the big one is covered by the single cuts.
void FrameTest::EveryPairOfCutsInTheSmallFrames() {
    QList<Frame> frames = SampleFrames();
    frames.removeAt(3);
    const QByteArray stream = EncodeAll(frames);
    for (qsizetype first = 0; first <= stream.size(); ++first) {
        for (qsizetype second = first; second <= stream.size(); ++second) {
            if (!SameFrames(DecodeInPieces(stream, {first, second}), frames)) {
                QFAIL(qPrintable(
                    QStringLiteral("cuts at %1, %2").arg(first).arg(second)));
            }
        }
    }
}

void FrameTest::ByteByByte() {
    const QList<Frame> frames = SampleFrames();
    const QByteArray stream = EncodeAll(frames);
    QList<qsizetype> cuts;
    for (qsizetype i = 1; i < stream.size(); ++i) {
        cuts.append(i);
    }
    QVERIFY(SameFrames(DecodeInPieces(stream, cuts), frames));
}

void FrameTest::RandomPieces() {
    const QList<Frame> frames = SampleFrames();
    const QByteArray stream = EncodeAll(frames);
    QRandomGenerator random(20261115);  // fixed: a failure must reproduce
    for (int round = 0; round < 200; ++round) {
        QList<qsizetype> cuts;
        qsizetype at = 0;
        while (true) {
            at += random.bounded(1, 4096);
            if (at >= stream.size()) {
                break;
            }
            cuts.append(at);
        }
        if (!SameFrames(DecodeInPieces(stream, cuts), frames)) {
            QFAIL(qPrintable(QStringLiteral("round %1").arg(round)));
        }
    }
}

void FrameTest::ZeroLengthFails() {
    FrameDecoder decoder;
    decoder.Feed(QByteArray(4, '\0'));
    QVERIFY(!decoder.Next());
    QVERIFY(decoder.Failed());
}

void FrameTest::OversizedLengthFailsBeforeTheBody() {
    FrameDecoder decoder;
    decoder.Feed(Header(roost::MAX_FRAME_LENGTH + 1, 0).first(4));
    QVERIFY(!decoder.Next());
    QVERIFY(decoder.Failed());

    // The limit itself is still allowed: it only needs the bytes.
    FrameDecoder atLimit;
    atLimit.Feed(Header(roost::MAX_FRAME_LENGTH, 0));
    QVERIFY(!atLimit.Next());
    QVERIFY(!atLimit.Failed());
}

void FrameTest::UnknownKindFailsBeforeTheBody() {
    FrameDecoder decoder;
    decoder.Feed(Header(100, 4));
    QVERIFY(!decoder.Next());
    QVERIFY(decoder.Failed());
    QVERIFY(decoder.Error().contains(QStringLiteral("kind 4")));
}

void FrameTest::FailureIsFinal() {
    FrameDecoder decoder;
    decoder.Feed(Header(1, 9));
    QVERIFY(!decoder.Next());
    decoder.Feed(roost::EncodeFrame(EFrameKind::Rpc, "{}"));
    QVERIFY(!decoder.Next());
    QVERIFY(decoder.Failed());
}

QTEST_GUILESS_MAIN(FrameTest)
#include "frame_test.moc"
