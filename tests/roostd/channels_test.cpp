// Frames of session channels (design.md §4) between RpcClient and Server:
// input reaches the server with the connection it came from, output,
// snapshots and notifications reach the client in the order they were sent,
// and a channel frame that breaks the protocol drops the connection.

#include <QEventLoop>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtEndian>

#include <cstdint>
#include <memory>
#include <optional>

#include "core/frame.hpp"
#include "core/protocol.hpp"
#include "core/rpc_client.hpp"
#include "core/version.hpp"
#include "roostd/server.hpp"

using namespace roost;

namespace {

constexpr int TIMEOUT_MS = 5000;

template <typename TValue>
bool WaitFor(const QFuture<TValue>& future) {
    if (future.isFinished()) {
        return true;
    }
    QEventLoop loop;
    QFutureWatcher<TValue> watcher;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop,
                     &QEventLoop::quit);
    QTimer::singleShot(TIMEOUT_MS, &loop, &QEventLoop::quit);
    watcher.setFuture(future);
    loop.exec();
    return future.isFinished();
}

// A socket that writes frames by hand, for what RpcClient never sends.
class RawPeer {
public:
    bool Connect(const QString& path) {
        Socket_.connectToServer(path);
        return QTest::qWaitFor(
            [&] { return Socket_.state() == QLocalSocket::ConnectedState; },
            TIMEOUT_MS);
    }

    void SendFrame(EFrameKind kind, const QByteArray& body) {
        Socket_.write(EncodeFrame(kind, body).value());
    }

    void Call(int id, const QString& method, const QJsonObject& params) {
        SendFrame(EFrameKind::Rpc,
                  QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                            {"id", id},
                                            {"method", method},
                                            {"params", params}})
                      .toJson(QJsonDocument::Compact));
    }

    void Greet() {
        Call(0, "hello",
             QJsonObject{
                 {"protocol", protocol::VERSION},
                 {"ghosttyVt", QString::fromStdString(GhosttyVtVersion())},
             });
    }

    // The body of the next frame, or nothing on timeout.
    std::optional<QByteArray> Receive() {
        std::optional<Frame> frame;
        const bool arrived = QTest::qWaitFor(
            [&] {
                Decoder_.Feed(Socket_.readAll());
                frame = Decoder_.Next();
                return frame.has_value();
            },
            TIMEOUT_MS);
        return arrived ? std::optional(frame->Body) : std::nullopt;
    }

    bool WaitClosed() {
        return QTest::qWaitFor(
            [&] { return Socket_.state() == QLocalSocket::UnconnectedState; },
            TIMEOUT_MS);
    }

private:
    QLocalSocket Socket_;
    FrameDecoder Decoder_;
};

// Takes warnings into a list instead of the test log while it lives.
class WarningCapture {
public:
    WarningCapture() {
        CAPTURED.clear();
        Previous_ = qInstallMessageHandler(
            [](QtMsgType type, const QMessageLogContext&, const QString& text) {
                if (type == QtWarningMsg) {
                    CAPTURED.append(text);
                }
            });
    }
    ~WarningCapture() { qInstallMessageHandler(Previous_); }

    WarningCapture(const WarningCapture&) = delete;
    WarningCapture& operator=(const WarningCapture&) = delete;

    const QStringList& Warnings() const { return CAPTURED; }

private:
    static inline QStringList CAPTURED;
    QtMessageHandler Previous_ = nullptr;
};

QByteArray ChannelBodyOf(std::uint32_t channel, const QByteArray& bytes) {
    QByteArray body(4, Qt::Uninitialized);
    qToBigEndian(channel, body.data());
    return body + bytes;
}

}  // namespace

class ChannelsTest : public QObject {
    Q_OBJECT

private:
    QString SocketPath() const { return Dir_->filePath("roostd.sock"); }

    // A server with three methods:
    //   test.peer remembers the connection that calls it in Id_;
    //   test.openNow sends a snapshot on channel 5 and answers {channel: 5};
    //   test.openLater answers {channel: 6} from the event loop, then sends
    //   a snapshot on channel 6.
    std::unique_ptr<Server> Start() {
        auto server = std::make_unique<Server>();
        const auto listening = server->Listen(SocketPath());
        if (!listening) {
            qWarning("%s", qUtf8Printable(listening.error()));
            return nullptr;
        }
        Server* raw = server.get();
        const bool registered =
            server->Register("test.peer",
                             [this, raw](const rpc::Call& call) {
                                 Id_ = raw->ConnectionOf(call.Peer);
                                 return rpc::Ready(QJsonValue());
                             }) &&
            server->Register("test.openNow",
                             [raw](const rpc::Call& call) {
                                 raw->SendToChannel(
                                     raw->ConnectionOf(call.Peer).value(),
                                     EFrameKind::Snapshot, 5, "now");
                                 return rpc::Ready(QJsonObject{{"channel", 5}});
                             }) &&
            server->Register("test.openLater", [raw](const rpc::Call& call) {
                const Server::ConnectionId id =
                    raw->ConnectionOf(call.Peer).value();
                auto promise = std::make_shared<QPromise<rpc::Result>>();
                promise->start();
                QMetaObject::invokeMethod(
                    raw,
                    [raw, id, promise] {
                        promise->addResult(QJsonObject{{"channel", 6}});
                        promise->finish();
                        raw->SendToChannel(id, EFrameKind::Snapshot, 6,
                                           "later");
                    },
                    Qt::QueuedConnection);
                return promise->future();
            });
        return registered ? std::move(server) : nullptr;
    }

    // A greeted client whose connection is Id_.
    std::unique_ptr<RpcClient> Connected() {
        Id_.reset();
        auto client = std::make_unique<RpcClient>();
        QFuture<rpc::Result> hello = client->Connect(SocketPath());
        if (!WaitFor(hello) || !hello.result()) {
            return nullptr;
        }
        QFuture<rpc::Result> peer = client->Request("test.peer");
        if (!WaitFor(peer) || !peer.result() || !Id_) {
            return nullptr;
        }
        return client;
    }

    std::unique_ptr<QTemporaryDir> Dir_;
    std::optional<Server::ConnectionId> Id_;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
        Id_.reset();
    }

    // --- the codec -------------------------------------------------------

    void TheChannelGoesFirstBigEndian() {
        QCOMPARE(
            EncodeChannelFrame(EFrameKind::Output, 0x01020304U, "ab").value(),
            QByteArray("\x00\x00\x00\x07\x01\x01\x02\x03\x04"
                       "ab",
                       11));
        const std::optional<ChannelBody> body =
            DecodeChannelBody(QByteArray("\xff\xff\xff\xfe"
                                         "xyz",
                                         7));
        QVERIFY(body);
        QCOMPARE(body->Channel, 0xfffffffeU);
        QCOMPARE(body->Bytes, QByteArray("xyz"));
    }

    void ABodyNeedsAChannel() {
        QVERIFY(!DecodeChannelBody(QByteArray("\0\0\0", 3)));
        const std::optional<ChannelBody> empty =
            DecodeChannelBody(QByteArray("\0\0\0\5", 4));
        QVERIFY(empty);
        QCOMPARE(empty->Channel, 5U);
        QVERIFY(empty->Bytes.isEmpty());
    }

    void TheLargestChannelFrameFits() {
        const QByteArray largest(MAX_CHANNEL_BYTES, 'x');
        const std::optional<QByteArray> frame =
            EncodeChannelFrame(EFrameKind::Snapshot, 9, largest);
        QVERIFY(frame);
        FrameDecoder decoder;
        decoder.Feed(*frame);
        const std::optional<Frame> decoded = decoder.Next();
        QVERIFY(decoded);
        QCOMPARE(DecodeChannelBody(decoded->Body)->Bytes.size(),
                 largest.size());
        QVERIFY(!EncodeChannelFrame(EFrameKind::Snapshot, 9, largest + 'x'));
    }

    // --- input -------------------------------------------------------------

    void InputComesWithItsConnection() {
        const auto server = Start();
        QVERIFY(server);
        const auto client = Connected();
        QVERIFY(client);
        QSignalSpy input(server.get(), &Server::InputReceived);

        client->SendInput(7, "ls\r");
        client->SendInput(0, "");
        QTRY_COMPARE(input.count(), 2);
        QCOMPARE(input.at(0).at(0).value<Server::ConnectionId>(), *Id_);
        QCOMPARE(input.at(0).at(1).value<std::uint32_t>(), 7U);
        QCOMPARE(input.at(0).at(2).toByteArray(), QByteArray("ls\r"));
        QCOMPARE(input.at(1).at(1).value<std::uint32_t>(), 0U);
        QVERIFY(input.at(1).at(2).toByteArray().isEmpty());
    }

    void LongInputIsCutIntoFrames() {
        const auto server = Start();
        QVERIFY(server);
        const auto client = Connected();
        QVERIFY(client);
        QByteArray paste(MAX_CHANNEL_BYTES + 10, 'p');
        paste[MAX_CHANNEL_BYTES] = 'q';
        QByteArray received;
        int frames = 0;
        connect(server.get(), &Server::InputReceived,
                [&](Server::ConnectionId, std::uint32_t channel,
                    const QByteArray& bytes) {
                    QCOMPARE(channel, 3U);
                    received += bytes;
                    ++frames;
                });

        client->SendInput(3, paste);
        QTRY_COMPARE_WITH_TIMEOUT(received.size(), paste.size(), TIMEOUT_MS);
        QCOMPARE(frames, 2);
        QCOMPARE(received, paste);
    }

    void InputBeforeHelloIsDropped() {
        const auto server = Start();
        QVERIFY(server);
        QSignalSpy input(server.get(), &Server::InputReceived);
        RawPeer peer;
        QVERIFY(peer.Connect(SocketPath()));

        peer.SendFrame(EFrameKind::Input, ChannelBodyOf(1, "early"));
        peer.Greet();
        QVERIFY(peer.Receive());
        peer.SendFrame(EFrameKind::Input, ChannelBodyOf(1, "late"));
        QTRY_COMPARE(input.count(), 1);
        QCOMPARE(input.at(0).at(2).toByteArray(), QByteArray("late"));
        QCOMPARE(server->ConnectionCount(), 1);
    }

    void InputBeforeTheClientConnectsIsDropped() {
        const auto server = Start();
        QVERIFY(server);
        QSignalSpy input(server.get(), &Server::InputReceived);
        // Not even a write to the closed socket.
        QTest::failOnWarning(QRegularExpression(".*"));
        RpcClient client;
        client.SendInput(1, "not connected");
        QVERIFY(WaitFor(client.Connect(SocketPath())));
        client.SendInput(1, "connected");
        QTRY_COMPARE(input.count(), 1);
        QCOMPARE(input.at(0).at(2).toByteArray(), QByteArray("connected"));
        client.Disconnect();
        client.SendInput(1, "lost");
    }

    void BadChannelFramesCloseTheConnection_data() {
        QTest::addColumn<int>("kind");
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("input without a channel")
            << static_cast<int>(EFrameKind::Input) << QByteArray("\0\0\1", 3);
        QTest::newRow("output from the peer")
            << static_cast<int>(EFrameKind::Output) << ChannelBodyOf(1, "x");
        QTest::newRow("snapshot from the peer")
            << static_cast<int>(EFrameKind::Snapshot) << ChannelBodyOf(1, "x");
    }

    void BadChannelFramesCloseTheConnection() {
        QFETCH(int, kind);
        QFETCH(QByteArray, body);
        const auto server = Start();
        QVERIFY(server);
        const auto bystander = Connected();
        QVERIFY(bystander);
        RawPeer peer;
        QVERIFY(peer.Connect(SocketPath()));
        peer.Greet();
        QVERIFY(peer.Receive());
        QTRY_COMPARE(server->ConnectionCount(), 2);

        QTest::ignoreMessage(
            QtWarningMsg, QRegularExpression("^roostd: closing a connection"));
        peer.SendFrame(static_cast<EFrameKind>(kind), body);
        QVERIFY(peer.WaitClosed());
        QTRY_COMPARE(server->ConnectionCount(), 1);
        QVERIFY(bystander->IsConnected());
    }

    // --- output ------------------------------------------------------------

    void FramesArriveInTheOrderSent() {
        const auto server = Start();
        QVERIFY(server);
        const auto client = Connected();
        QVERIFY(client);
        QStringList log;
        connect(client.get(), &RpcClient::OutputReceived,
                [&](std::uint32_t channel, const QByteArray& bytes) {
                    log << QString("output %1 %2").arg(channel).arg(bytes);
                });
        connect(client.get(), &RpcClient::SnapshotReceived,
                [&](std::uint32_t channel, const QByteArray& bytes) {
                    log << QString("snapshot %1 %2").arg(channel).arg(bytes);
                });
        connect(client.get(), &RpcClient::NotificationReceived,
                [&](const QString& method, const QJsonValue& params) {
                    log << QString("%1 %2").arg(method).arg(
                        params.toObject().value("channel").toInt());
                });

        QVERIFY(server->SendToChannel(*Id_, EFrameKind::Snapshot, 4, "snap"));
        QVERIFY(server->SendToChannel(*Id_, EFrameKind::Output, 4, "out"));
        QVERIFY(server->Notify(*Id_, "session.closed",
                               QJsonObject{{"channel", 4}}));
        QVERIFY(server->SendToChannel(*Id_, EFrameKind::Output, 0, ""));
        QTRY_COMPARE(log.size(), 4);
        QCOMPARE(log, (QStringList{"snapshot 4 snap", "output 4 out",
                                   "session.closed 4", "output 0 "}));
    }

    void OnlyAConnectionOfTheServerIsNamed() {
        const auto server = Start();
        QVERIFY(server);
        QObject stranger;
        QVERIFY(!server->ConnectionOf(nullptr));
        QVERIFY(!server->ConnectionOf(&stranger));
        QVERIFY(!server->SendToChannel(0, EFrameKind::Output, 1, "x"));
        QVERIFY(!server->Notify(0, "session.closed", QJsonObject{}));

        // A connection of another server is no connection of this one.
        Server other;
        QVERIFY(other.Listen(Dir_->filePath("other.sock")));
        std::optional<std::optional<Server::ConnectionId>> crossed;
        QVERIFY(other.Register("test.cross", [&](const rpc::Call& call) {
            crossed = server->ConnectionOf(call.Peer);
            return rpc::Ready(QJsonValue());
        }));
        RpcClient client;
        QVERIFY(WaitFor(client.Connect(Dir_->filePath("other.sock"))));
        QVERIFY(WaitFor(client.Request("test.cross")));
        QVERIFY(crossed);
        QVERIFY(!*crossed);
    }

    void AClosedConnectionStaysClosed() {
        const auto server = Start();
        QVERIFY(server);
        QSignalSpy closed(server.get(), &Server::ConnectionClosed);
        const auto first = Connected();
        QVERIFY(first);
        const Server::ConnectionId gone = *Id_;
        first->Disconnect();
        QTRY_COMPARE(closed.count(), 1);
        QCOMPARE(closed.at(0).at(0).value<Server::ConnectionId>(), gone);
        QVERIFY(!server->SendToChannel(gone, EFrameKind::Output, 1, "x"));
        QVERIFY(!server->Notify(gone, "session.closed", QJsonObject{}));

        // The next connection may take the memory of the last one, never its
        // id.
        const auto second = Connected();
        QVERIFY(second);
        QVERIFY(*Id_ != gone);
        QSignalSpy output(second.get(), &RpcClient::OutputReceived);
        QVERIFY(!server->SendToChannel(gone, EFrameKind::Output, 1, "stale"));
        QVERIFY(server->SendToChannel(*Id_, EFrameKind::Output, 1, "fresh"));
        QTRY_COMPARE(output.count(), 1);
        QCOMPARE(output.at(0).at(1).toByteArray(), QByteArray("fresh"));
        QCOMPARE(closed.count(), 1);
    }

    void AConnectionTheServerClosesIsReportedOnce() {
        const auto server = Start();
        QVERIFY(server);
        RawPeer peer;
        QVERIFY(peer.Connect(SocketPath()));
        peer.Greet();
        QVERIFY(peer.Receive());
        peer.Call(1, "test.peer", QJsonObject{});
        QVERIFY(peer.Receive());
        QVERIFY(Id_);
        QSignalSpy closed(server.get(), &Server::ConnectionClosed);

        QTest::ignoreMessage(
            QtWarningMsg, QRegularExpression("^roostd: closing a connection"));
        peer.SendFrame(EFrameKind::Output, ChannelBodyOf(1, "x"));
        QTRY_COMPARE(closed.count(), 1);
        QCOMPARE(closed.at(0).at(0).value<Server::ConnectionId>(), *Id_);
        QVERIFY(!server->SendToChannel(*Id_, EFrameKind::Output, 1, "x"));
        QVERIFY(peer.WaitClosed());
        QTest::qWait(50);
        QCOMPARE(closed.count(), 1);
    }

    void TheFrameThatOverflowsAPeerIsRefused() {
        const auto server = Start();
        QVERIFY(server);
        RawPeer peer;
        QVERIFY(peer.Connect(SocketPath()));
        peer.Greet();
        QVERIFY(peer.Receive());
        peer.Call(1, "test.peer", QJsonObject{});
        QVERIFY(peer.Receive());
        QVERIFY(Id_);
        QSignalSpy closed(server.get(), &Server::ConnectionClosed);

        // The peer reads no more; MAX_UNSENT_BYTES is four such frames. The
        // call whose frame closes the connection is the first to say false.
        QStringList warnings;
        int sent = 0;
        {
            const WarningCapture capture;
            const QByteArray big(MAX_CHANNEL_BYTES, 'b');
            while (sent < 10 &&
                   server->SendToChannel(*Id_, EFrameKind::Output, 1, big)) {
                QVERIFY2(capture.Warnings().isEmpty(),
                         qPrintable(capture.Warnings().join('\n')));
                ++sent;
            }
            warnings = capture.Warnings();
        }
        QVERIFY2(sent >= 3 && sent < 10, qPrintable(QString::number(sent)));
        QCOMPARE(warnings.size(), 1);
        QVERIFY2(warnings[0].startsWith("roostd: closing a connection: the "
                                        "peer does not read"),
                 qPrintable(warnings[0]));
        QVERIFY(!server->SendToChannel(*Id_, EFrameKind::Output, 1, "x"));
        QTRY_COMPARE(closed.count(), 1);
    }

    void TheLargestChannelFrameGoesThrough() {
        const auto server = Start();
        QVERIFY(server);
        const auto client = Connected();
        QVERIFY(client);
        QSignalSpy snapshot(client.get(), &RpcClient::SnapshotReceived);
        const QByteArray largest(MAX_CHANNEL_BYTES, 's');
        QVERIFY(server->SendToChannel(*Id_, EFrameKind::Snapshot, 2, largest));
        QTRY_COMPARE_WITH_TIMEOUT(snapshot.count(), 1, TIMEOUT_MS);
        QCOMPARE(snapshot.at(0).at(0).value<std::uint32_t>(), 2U);
        QCOMPARE(snapshot.at(0).at(1).toByteArray(), largest);
    }

    void AFrameFromAHandlerGoesBeforeItsAnswer_data() {
        QTest::addColumn<QString>("method");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("sent by the handler")
            << "test.openNow" << QStringList{"snapshot 5 now", "answer 5"};
        QTest::newRow("sent after finish")
            << "test.openLater" << QStringList{"answer 6", "snapshot 6 later"};
    }

    void AFrameFromAHandlerGoesBeforeItsAnswer() {
        QFETCH(QString, method);
        QFETCH(QStringList, expected);
        const auto server = Start();
        QVERIFY(server);
        const auto client = Connected();
        QVERIFY(client);
        QStringList log;
        connect(client.get(), &RpcClient::SnapshotReceived,
                [&](std::uint32_t channel, const QByteArray& bytes) {
                    log << QString("snapshot %1 %2").arg(channel).arg(bytes);
                });
        client->Request(method).then(client.get(), [&](const rpc::Result&
                                                           result) {
            log << QString("answer %1")
                       .arg(result.value().toObject().value("channel").toInt());
        });
        QTRY_COMPARE(log.size(), 2);
        QCOMPARE(log, expected);
    }

    void WhatDoesNotFitIsRefusedAndTheConnectionStays() {
        const auto server = Start();
        QVERIFY(server);
        const auto client = Connected();
        QVERIFY(client);
        QSignalSpy output(client.get(), &RpcClient::OutputReceived);

        QVERIFY(!server->SendToChannel(*Id_, EFrameKind::Snapshot, 1,
                                       QByteArray(MAX_CHANNEL_BYTES + 1, 's')));
        QVERIFY(!server->Notify(
            *Id_, "session.big",
            QString(static_cast<qsizetype>(MAX_FRAME_LENGTH), u'n')));
        QVERIFY(server->SendToChannel(*Id_, EFrameKind::Output, 1, "after"));
        QTRY_COMPARE(output.count(), 1);
        QCOMPARE(output.at(0).at(1).toByteArray(), QByteArray("after"));
        QVERIFY(client->IsConnected());
    }

    void BadFramesFromRoostdLoseTheConnection_data() {
        QTest::addColumn<QByteArray>("frame");
        QTest::addColumn<QString>("reason");
        QTest::newRow("input")
            << EncodeChannelFrame(EFrameKind::Input, 1, "x").value()
            << "roostd sent an input frame";
        QTest::newRow("output without a channel")
            << EncodeFrame(EFrameKind::Output, QByteArray("\0\0", 2)).value()
            << "roostd sent a channel frame without a channel";
        QTest::newRow("snapshot without a channel")
            << EncodeFrame(EFrameKind::Snapshot, QByteArray()).value()
            << "roostd sent a channel frame without a channel";
    }

    void BadFramesFromRoostdLoseTheConnection() {
        QFETCH(QByteArray, frame);
        QFETCH(QString, reason);
        // A broken roostd: the bad frame comes before any answer to hello.
        QLocalServer fake;
        QVERIFY(fake.listen(SocketPath()));
        connect(&fake, &QLocalServer::newConnection,
                [&] { fake.nextPendingConnection()->write(frame); });

        RpcClient client;
        QSignalSpy disconnected(&client, &RpcClient::Disconnected);
        QSignalSpy output(&client, &RpcClient::OutputReceived);
        QSignalSpy snapshot(&client, &RpcClient::SnapshotReceived);
        QFuture<rpc::Result> hello = client.Connect(SocketPath());
        QVERIFY(WaitFor(hello));
        QCOMPARE(hello.result().error().Code,
                 static_cast<int>(protocol::EErrorCode::ConnectionLost));
        QCOMPARE(disconnected.count(), 1);
        QCOMPARE(disconnected.at(0).at(0).toString(), reason);
        QCOMPARE(output.count(), 0);
        QCOMPARE(snapshot.count(), 0);
    }
};

QTEST_GUILESS_MAIN(ChannelsTest)
#include "channels_test.moc"
