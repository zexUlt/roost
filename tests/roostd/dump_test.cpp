#include <QTest>
#include <QtEndian>

#include "roostd/dump.hpp"

using namespace roost;

namespace {

QByteArray Channel(quint32 channel, const QByteArray& data) {
    QByteArray body(4, '\0');
    qToBigEndian(channel, body.data());
    return body + data;
}

}  // namespace

class DumpTest : public QObject {
    Q_OBJECT

private slots:
    void FramesAreDescribed_data() {
        QTest::addColumn<int>("kind");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QString>("line");
        QTest::newRow("rpc, made compact")
            << 0 << QByteArray("{ \"id\": 1,\n \"result\": null }")
            << "rpc {\"id\":1,\"result\":null}";
        QTest::newRow("rpc batch")
            << 0 << QByteArray("[{\"id\":1}]") << "rpc [{\"id\":1}]";
        QTest::newRow("rpc, not JSON")
            << 0 << QByteArray("{\"x\n") << "rpc (not JSON) \"{\\\"x\\n\"";
        QTest::newRow("output")
            << 1 << Channel(7, "ls\r\n\x1b[0m")
            << "output channel 7, 8 bytes \"ls\\r\\n\\x1b[0m\"";
        QTest::newRow("input, big channel")
            << 2 << Channel(0xfffffffe, "q")
            << "input channel 4294967294, 1 bytes \"q\"";
        QTest::newRow("snapshot, long")
            << 3 << Channel(1, QByteArray(100, 'x'))
            << QString("snapshot channel 1, 100 bytes \"%1\"...")
                   .arg(QString(64, u'x'));
        QTest::newRow("snapshot, exactly the preview")
            << 3 << Channel(1, QByteArray(64, 'x'))
            << QString("snapshot channel 1, 64 bytes \"%1\"")
                   .arg(QString(64, u'x'));
        QTest::newRow("no channel")
            << 1 << QByteArray("abc") << "output (no channel) 3 bytes";
        QTest::newRow("empty channel frame")
            << 1 << Channel(3, {}) << "output channel 3, 0 bytes \"\"";
        QTest::newRow("tab, quote, backslash, high byte")
            << 1 << Channel(1, "\t\"\\\xff")
            << "output channel 1, 4 bytes \"\\t\\\"\\\\\\xff\"";
        QTest::newRow("control bytes")
            << 1 << Channel(1, QByteArray("\x01\x7f", 2))
            << "output channel 1, 2 bytes \"\\x01\\x7f\"";
    }

    void FramesAreDescribed() {
        QFETCH(int, kind);
        QFETCH(QByteArray, body);
        QFETCH(QString, line);
        QCOMPARE(DescribeFrame(Frame{.Kind = static_cast<EFrameKind>(kind),
                                     .Body = body}),
                 line);
    }
};

QTEST_GUILESS_MAIN(DumpTest)
#include "dump_test.moc"
