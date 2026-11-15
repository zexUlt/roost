#include <QDeadlineTimer>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <optional>

#include "roostd/dump.hpp"

using namespace roost;
using namespace std::chrono_literals;

namespace {

constexpr int TIMEOUT_MS = 5000;

QByteArray RpcFrame(const QByteArray& body) {
    return EncodeFrame(EFrameKind::Rpc, body).value();
}

using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;

File TemporaryFile() { return {std::tmpfile(), &std::fclose}; }

QByteArray ReadBack(std::FILE* file) {
    QFile written;
    if (!written.open(file, QIODevice::ReadOnly) || !written.seek(0)) {
        return {};
    }
    return written.readAll();
}

// Runs Dump against a fake roostd that sends the bytes to it, then closes the
// connection or keeps it open. The exit code, or nothing if Dump hung.
std::optional<int> DumpAgainst(const QByteArray& sent, bool close,
                               std::FILE* out) {
    const QTemporaryDir dir;
    const QString path = dir.filePath("roostd.sock");
    QLocalServer server;
    if (!server.listen(path)) {
        return std::nullopt;
    }
    std::future<int> dumped =
        std::async(std::launch::async, [&] { return Dump(path, out); });
    if (!server.waitForNewConnection(TIMEOUT_MS)) {
        return std::nullopt;
    }
    QLocalSocket* client = server.nextPendingConnection();
    // As roostd does, answers only once the dump has said all it says.
    QByteArray received;
    while (!received.contains("bus.subscribe") &&
           client->waitForReadyRead(TIMEOUT_MS)) {
        received += client->readAll();
    }
    client->write(sent);
    if (close) {
        client->disconnectFromServer();
    }
    const QDeadlineTimer deadline(TIMEOUT_MS);
    while (dumped.wait_for(0s) != std::future_status::ready &&
           !deadline.hasExpired()) {
        QTest::qWait(10);
    }
    if (dumped.wait_for(0s) != std::future_status::ready) {
        // Let it out before giving up on it.
        client->abort();
        dumped.wait();
        return std::nullopt;
    }
    return dumped.get();
}

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
        QTest::newRow("rpc") << 0 << QByteArray("{\"id\":1,\"result\":null}")
                             << "rpc {\"id\":1,\"result\":null}";
        QTest::newRow("rpc batch")
            << 0 << QByteArray("[{\"id\":1}]") << "rpc [{\"id\":1}]";
        QTest::newRow("rpc, as sent")
            << 0 << QByteArray("{\"d\":1,\"d\":2,\"n\":12345678901234567890}")
            << "rpc {\"d\":1,\"d\":2,\"n\":12345678901234567890}";
        QTest::newRow("rpc, a scalar") << 0 << QByteArray("null") << "rpc null";
        QTest::newRow("rpc, on lines")
            << 0 << QByteArray("{\n\t\"a\": 1\r\n}") << "rpc {  \"a\": 1  }";
        QTest::newRow("rpc, two values")
            << 0 << QByteArray("1,2") << "rpc (not JSON) \"1,2\"";
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

    void DumpEndsWithTheConnection() {
        const File out = TemporaryFile();
        QCOMPARE(
            DumpAgainst(RpcFrame("{\"id\":1,\"result\":{}}"), true, out.get()),
            0);
        QCOMPARE(ReadBack(out.get()), "rpc {\"id\":1,\"result\":{}}\n");
    }

    void DumpFailsOnAFrameCutShort() {
        const File out = TemporaryFile();
        const QByteArray cut = RpcFrame(QByteArray(100, ' ')).first(20);
        QCOMPARE(DumpAgainst(RpcFrame("null") + cut, true, out.get()), 1);
        QCOMPARE(ReadBack(out.get()), "rpc null\n");
    }

    void DumpFailsWhenRefused_data() {
        QTest::addColumn<int>("id");
        QTest::newRow("hello") << 1;
        QTest::newRow("bus.subscribe") << 2;
    }

    void DumpFailsWhenRefused() {
        QFETCH(int, id);
        const File out = TemporaryFile();
        const QByteArray refusal =
            QStringLiteral("{\"id\":%1,\"error\":{\"code\":-32001}}")
                .arg(id)
                .toUtf8();
        // Not waiting for roostd to hang up.
        QCOMPARE(DumpAgainst(RpcFrame(refusal), false, out.get()), 1);
    }

    void DumpEndsWhenItsReaderGoesAway() {
        int ends[2] = {-1, -1};
        QCOMPARE(::pipe(ends), 0);
        ::close(ends[0]);
        const File out(::fdopen(ends[1], "w"), &std::fclose);
        QVERIFY(out);
        QCOMPARE(DumpAgainst(RpcFrame("null"), false, out.get()), 0);
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
