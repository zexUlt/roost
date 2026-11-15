#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QPromise>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
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

// Runs the event loop until the future finishes. Not QTest::qWaitFor: it
// sleeps between rounds, and a big message takes hundreds of them, as the
// socket moves a few kilobytes at a time.
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

int CodeOf(const rpc::Result& result) {
    return result ? 0 : result.error().Code;
}

constexpr int Code(protocol::EErrorCode code) { return static_cast<int>(code); }

constexpr int Code(rpc::EErrorCode code) { return static_cast<int>(code); }

// A socket that speaks frames by hand, for what RpcClient would never send.
// Waits spin the event loop: the server runs on the same thread.
class RawPeer {
public:
    bool Connect(const QString& path) {
        Socket_.connectToServer(path);
        return QTest::qWaitFor(
            [&] { return Socket_.state() == QLocalSocket::ConnectedState; },
            TIMEOUT_MS);
    }

    QLocalSocket& Socket() { return Socket_; }

    void SendBytes(const QByteArray& bytes) { Socket_.write(bytes); }

    void SendFrame(EFrameKind kind, const QByteArray& body) {
        SendBytes(EncodeFrame(kind, body).value());
    }

    void Send(const QJsonValue& message) {
        const QByteArray body = message.isArray()
                                    ? QJsonDocument(message.toArray())
                                          .toJson(QJsonDocument::Compact)
                                    : QJsonDocument(message.toObject())
                                          .toJson(QJsonDocument::Compact);
        SendFrame(EFrameKind::Rpc, body);
    }

    void Call(int id, const QString& method,
              const QJsonValue& params = QJsonValue::Undefined) {
        QJsonObject request{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
        if (!params.isUndefined()) {
            request.insert("params", params);
        }
        Send(request);
    }

    void Greet() {
        Call(0, "hello",
             QJsonObject{
                 {"protocol", protocol::VERSION},
                 {"ghosttyVt", QString::fromStdString(GhosttyVtVersion())},
             });
    }

    // The next message, or nothing on timeout.
    std::optional<QJsonValue> Receive() {
        std::optional<Frame> frame;
        const bool arrived = QTest::qWaitFor(
            [&] {
                Decoder_.Feed(Socket_.readAll());
                frame = Decoder_.Next();
                return frame.has_value();
            },
            TIMEOUT_MS);
        if (!arrived) {
            return std::nullopt;
        }
        const QJsonDocument document = QJsonDocument::fromJson(frame->Body);
        return document.isArray() ? QJsonValue(document.array())
                                  : QJsonValue(document.object());
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

}  // namespace

class ServerTest : public QObject {
    Q_OBJECT

private:
    QString SocketPath() const { return Dir_->filePath("roostd.sock"); }

    // A server listening at SocketPath(), with test.echo registered and
    // test.wait answering through Waiting_.
    std::unique_ptr<Server> Start() {
        auto server = std::make_unique<Server>();
        const auto listening = server->Listen(SocketPath());
        if (!listening) {
            qWarning("%s", qUtf8Printable(listening.error()));
            return nullptr;
        }
        const bool echo = server->Register(
            "test.echo",
            [](const rpc::Call& call) { return rpc::Ready(call.Params); });
        const bool wait =
            server->Register("test.wait", [this](const rpc::Call&) {
                Waiting_ = std::make_shared<QPromise<rpc::Result>>();
                Waiting_->start();
                return Waiting_->future();
            });
        return echo && wait ? std::move(server) : nullptr;
    }

    std::unique_ptr<RpcClient> Connected() {
        auto client = std::make_unique<RpcClient>();
        QFuture<rpc::Result> hello = client->Connect(SocketPath());
        if (!WaitFor(hello) || !hello.result()) {
            return nullptr;
        }
        return client;
    }

    std::unique_ptr<QTemporaryDir> Dir_;
    std::shared_ptr<QPromise<rpc::Result>> Waiting_;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
        Waiting_.reset();
    }

    void cleanup() {
        // A promise left unfinished must not outlive the test that made it.
        if (Waiting_) {
            Waiting_->finish();
        }
        Waiting_.reset();
    }

    // --- hello -----------------------------------------------------------

    void HelloAnswersWithTheVersions() {
        const auto server = Start();
        QVERIFY(server);
        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(SocketPath());
        QVERIFY(WaitFor(hello));
        const rpc::Result result = hello.result();
        QVERIFY(result);
        QCOMPARE(result->toObject(),
                 (QJsonObject{
                     {"protocol", protocol::VERSION},
                     {"ghosttyVt", QString::fromStdString(GhosttyVtVersion())},
                     {"roostd", QString::fromStdString(VersionString())},
                 }));
        QVERIFY(client.IsConnected());
    }

    void HelloWithAnotherBuildIsRefused_data() {
        QTest::addColumn<QJsonValue>("protocol");
        QTest::addColumn<QString>("ghosttyVt");
        const QString ours = QString::fromStdString(GhosttyVtVersion());
        QTest::newRow("newer protocol")
            << QJsonValue(protocol::VERSION + 1) << ours;
        QTest::newRow("older protocol")
            << QJsonValue(protocol::VERSION - 1) << ours;
        QTest::newRow("other library")
            << QJsonValue(protocol::VERSION) << QString("0.0.1");
    }

    void HelloWithAnotherBuildIsRefused() {
        QFETCH(QJsonValue, protocol);
        QFETCH(QString, ghosttyVt);
        const auto server = Start();
        QVERIFY(server);
        RawPeer peer;
        QVERIFY(peer.Connect(SocketPath()));
        peer.Call(
            1, "hello",
            QJsonObject{{"protocol", protocol}, {"ghosttyVt", ghosttyVt}});
        const auto response = peer.Receive();
        QVERIFY(response);
        const QJsonObject error =
            response->toObject().value("error").toObject();
        QCOMPARE(error.value("code").toInt(),
                 Code(protocol::EErrorCode::VersionMismatch));
        QCOMPARE(error.value("data").toObject(),
                 (QJsonObject{
                     {"protocol", protocol::VERSION},
                     {"ghosttyVt", QString::fromStdString(GhosttyVtVersion())},
                 }));

        // Refused means not greeted.
        peer.Call(2, "test.echo");
        const auto after = peer.Receive();
        QVERIFY(after);
        QCOMPARE(
            after->toObject().value("error").toObject().value("code").toInt(),
            Code(protocol::EErrorCode::HandshakeRequired));
    }

    void HelloWithBadParamsIsInvalid_data() {
        QTest::addColumn<QJsonValue>("params");
        const QString ours = QString::fromStdString(GhosttyVtVersion());
        QTest::newRow("absent") << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("array")
            << QJsonValue(QJsonArray{protocol::VERSION, ours});
        QTest::newRow("no protocol")
            << QJsonValue(QJsonObject{{"ghosttyVt", ours}});
        QTest::newRow("no library")
            << QJsonValue(QJsonObject{{"protocol", protocol::VERSION}});
        QTest::newRow("protocol as string")
            << QJsonValue(QJsonObject{{"protocol", "1"}, {"ghosttyVt", ours}});
        QTest::newRow("fractional protocol")
            << QJsonValue(QJsonObject{{"protocol", 1.5}, {"ghosttyVt", ours}});
        QTest::newRow("library as number") << QJsonValue(
            QJsonObject{{"protocol", protocol::VERSION}, {"ghosttyVt", 1}});
    }

    void HelloWithBadParamsIsInvalid() {
        QFETCH(QJsonValue, params);
        const auto server = Start();
        QVERIFY(server);
        RawPeer peer;
        QVERIFY(peer.Connect(SocketPath()));
        peer.Call(1, "hello", params);
        const auto response = peer.Receive();
        QVERIFY(response);
        QCOMPARE(response->toObject()
                     .value("error")
                     .toObject()
                     .value("code")
                     .toInt(),
                 Code(rpc::EErrorCode::InvalidParams));
    }

    void MethodsWaitForHello() {
        const auto server = Start();
        QVERIFY(server);
        RawPeer peer;
        QVERIFY(peer.Connect(SocketPath()));
        for (const char* method :
             {"test.echo", "bus.subscribe", "bus.unsubscribe", "bus.publish"}) {
            peer.Call(1, method, QJsonObject{});
            const auto response = peer.Receive();
            QVERIFY(response);
            QCOMPARE(response->toObject()
                         .value("error")
                         .toObject()
                         .value("code")
                         .toInt(),
                     Code(protocol::EErrorCode::HandshakeRequired));
        }
        // A method nobody serves is not found, greeted or not.
        peer.Call(2, "no.such");
        const auto unknown = peer.Receive();
        QVERIFY(unknown);
        QCOMPARE(
            unknown->toObject().value("error").toObject().value("code").toInt(),
            Code(rpc::EErrorCode::MethodNotFound));
    }

    void RequestsRightAfterHelloAreGreeted() {
        const auto server = Start();
        QVERIFY(server);
        RawPeer peer;
        QVERIFY(peer.Connect(SocketPath()));
        // Sent without waiting for hello's answer: frames are handled in
        // order, and so are the requests of a batch.
        peer.Greet();
        peer.Call(1, "test.echo", QJsonObject{{"x", 1}});
        const auto hello = peer.Receive();
        const auto echo = peer.Receive();
        QVERIFY(hello && echo);
        QCOMPARE(echo->toObject().value("result").toObject(),
                 (QJsonObject{{"x", 1}}));

        RawPeer batch;
        QVERIFY(batch.Connect(SocketPath()));
        batch.Send(QJsonArray{
            QJsonObject{{"jsonrpc", "2.0"},
                        {"id", 1},
                        {"method", "hello"},
                        {"params",
                         QJsonObject{{"protocol", protocol::VERSION},
                                     {"ghosttyVt", QString::fromStdString(
                                                       GhosttyVtVersion())}}}},
            QJsonObject{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "test.echo"}},
        });
        const auto responses = batch.Receive();
        QVERIFY(responses);
        const QJsonArray array = responses->toArray();
        QCOMPARE(array.size(), 2);
        QVERIFY(array.at(1).toObject().contains("result"));
    }

    void RegisterRefusesTakenAndReservedNames() {
        const auto server = Start();
        QVERIFY(server);
        const auto any = [](const rpc::Call&) {
            return rpc::Ready(QJsonValue());
        };
        QVERIFY(!server->Register("hello", any));
        QVERIFY(!server->Register("bus.publish", any));
        QVERIFY(!server->Register("test.echo", any));
        QVERIFY(!server->Register("rpc.anything", any));
    }

    // --- the stream ------------------------------------------------------

    void BrokenFramingClosesTheConnection_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("zero length") << QByteArray("\0\0\0\0", 4);
        QTest::newRow("unknown kind") << QByteArray("\0\0\0\1\x7f", 5);
        QTest::newRow("oversized") << QByteArray("\x7f\xff\xff\xff\0", 5);
        QTest::newRow("output frame")
            << EncodeFrame(EFrameKind::Output, QByteArray("\0\0\0\1x", 5))
                   .value();
    }

    void BrokenFramingClosesTheConnection() {
        QFETCH(QByteArray, bytes);
        const auto server = Start();
        QVERIFY(server);
        const auto bystander = Connected();
        QVERIFY(bystander);
        RawPeer peer;
        QVERIFY(peer.Connect(SocketPath()));
        QTRY_COMPARE(server->ConnectionCount(), 2);

        QTest::ignoreMessage(
            QtWarningMsg, QRegularExpression("^roostd: closing a connection"));
        peer.SendBytes(bytes);
        QVERIFY(peer.WaitClosed());
        QTRY_COMPARE(server->ConnectionCount(), 1);

        // Only that connection.
        QFuture<rpc::Result> echo =
            bystander->Request("test.echo", QJsonArray{7});
        QVERIFY(WaitFor(echo));
        QCOMPARE(echo.result().value(), QJsonValue(QJsonArray{7}));
    }

    void UnparsableMessageIsAnsweredNotClosed() {
        const auto server = Start();
        QVERIFY(server);
        RawPeer peer;
        QVERIFY(peer.Connect(SocketPath()));
        peer.SendFrame(EFrameKind::Rpc, "{not json");
        const auto response = peer.Receive();
        QVERIFY(response);
        QCOMPARE(response->toObject().value("id"), QJsonValue());
        QCOMPARE(response->toObject()
                     .value("error")
                     .toObject()
                     .value("code")
                     .toInt(),
                 Code(rpc::EErrorCode::ParseError));
        peer.Greet();
        QVERIFY(peer.Receive());
    }

    void FramesSplitAnywhereArrive() {
        const auto server = Start();
        QVERIFY(server);
        RawPeer peer;
        QVERIFY(peer.Connect(SocketPath()));
        QByteArray stream;
        stream +=
            EncodeFrame(
                EFrameKind::Rpc,
                QJsonDocument(QJsonObject{
                                  {"jsonrpc", "2.0"},
                                  {"id", 1},
                                  {"method", "hello"},
                                  {"params",
                                   QJsonObject{
                                       {"protocol", protocol::VERSION},
                                       {"ghosttyVt", QString::fromStdString(
                                                         GhosttyVtVersion())},
                                   }},
                              })
                    .toJson(QJsonDocument::Compact))
                .value();
        for (int id = 2; id < 5; ++id) {
            stream += EncodeFrame(EFrameKind::Rpc,
                                  QJsonDocument(QJsonObject{
                                                    {"jsonrpc", "2.0"},
                                                    {"id", id},
                                                    {"method", "test.echo"},
                                                    {"params", QJsonArray{id}},
                                                })
                                      .toJson(QJsonDocument::Compact))
                          .value();
        }
        // A byte at a time, each in its own write.
        for (const char byte : stream) {
            peer.SendBytes(QByteArray(1, byte));
            peer.Socket().flush();
            QCoreApplication::processEvents();
        }
        for (int id = 1; id < 5; ++id) {
            const auto response = peer.Receive();
            QVERIFY(response);
            QCOMPARE(response->toObject().value("id").toInt(), id);
        }
    }

    void LargeMessagesRoundTrip() {
        const auto server = Start();
        QVERIFY(server);
        const auto client = Connected();
        QVERIFY(client);
        const QString big(4 * 1024 * 1024, u'x');
        QFuture<rpc::Result> echo =
            client->Request("test.echo", QJsonArray{big});
        QVERIFY(WaitFor(echo));
        QCOMPARE(echo.result().value(), QJsonValue(QJsonArray{big}));
    }

    void AnswerTooBigForAFrameClosesTheConnection() {
        const auto server = Start();
        QVERIFY(server);
        QVERIFY(server->Register("test.huge", [](const rpc::Call&) {
            return rpc::Ready(QString(MAX_FRAME_LENGTH, u'x'));
        }));
        const auto client = Connected();
        QVERIFY(client);
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression("does not fit a frame"));
        QFuture<rpc::Result> huge = client->Request("test.huge");
        QVERIFY(WaitFor(huge));
        QCOMPARE(CodeOf(huge.result()),
                 Code(protocol::EErrorCode::ConnectionLost));
    }

    void AnswersComeOutOfOrder() {
        const auto server = Start();
        QVERIFY(server);
        const auto client = Connected();
        QVERIFY(client);
        QFuture<rpc::Result> slow = client->Request("test.wait");
        QFuture<rpc::Result> fast = client->Request("test.echo", QJsonArray{2});
        QVERIFY(WaitFor(fast));
        QCOMPARE(fast.result().value(), QJsonValue(QJsonArray{2}));
        QVERIFY(!slow.isFinished());
        QVERIFY(Waiting_);
        Waiting_->addResult(rpc::Result(QJsonValue(1)));
        Waiting_->finish();
        QVERIFY(WaitFor(slow));
        QCOMPARE(slow.result().value(), QJsonValue(1));
    }

    void ClientGoneWhileAHandlerWaits() {
        const auto server = Start();
        QVERIFY(server);
        auto client = Connected();
        QVERIFY(client);
        QFuture<rpc::Result> slow = client->Request("test.wait");
        QTRY_VERIFY(Waiting_);
        client->Disconnect();
        QVERIFY(WaitFor(slow));
        QCOMPARE(CodeOf(slow.result()),
                 Code(protocol::EErrorCode::ConnectionLost));
        QTRY_COMPARE(server->ConnectionCount(), 0);
        // The answer has nowhere to go and must not crash trying.
        Waiting_->addResult(rpc::Result(QJsonValue(1)));
        Waiting_->finish();
        QTest::qWait(10);
    }

    void ServerGoneWhileAHandlerWaits() {
        auto server = Start();
        QVERIFY(server);
        const auto client = Connected();
        QVERIFY(client);
        QSignalSpy disconnected(client.get(), &RpcClient::Disconnected);
        QFuture<rpc::Result> slow = client->Request("test.wait");
        QTRY_VERIFY(Waiting_);
        server.reset();
        QVERIFY(WaitFor(slow));
        QCOMPARE(CodeOf(slow.result()),
                 Code(protocol::EErrorCode::ConnectionLost));
        QCOMPARE(disconnected.count(), 1);
        Waiting_->addResult(rpc::Result(QJsonValue(1)));
        Waiting_->finish();
        QTest::qWait(10);
    }

    void SlowReaderIsDropped() {
        const auto server = Start();
        QVERIFY(server);
        RawPeer reader;
        QVERIFY(reader.Connect(SocketPath()));
        // Qt stops taking bytes off the socket once this much is buffered.
        reader.Socket().setReadBufferSize(1024);
        reader.Greet();
        reader.Call(1, "bus.subscribe", QJsonObject{{"pattern", "test.*"}});
        QTRY_COMPARE(server->Bus().SubscriptionCount(), size_t{1});

        const auto publisher = Connected();
        QVERIFY(publisher);
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression("does not read its messages"));
        const QString chunk(4 * 1024 * 1024, u'x');
        const qint64 needed = Server::MAX_UNSENT_BYTES / chunk.size() + 2;
        for (qint64 i = 0; i < needed; ++i) {
            QFuture<rpc::Result> published = publisher->Request(
                "bus.publish",
                QJsonObject{{"name", "test.big"}, {"payload", chunk}});
            QVERIFY(WaitFor(published));
        }
        QTRY_COMPARE(server->Bus().SubscriptionCount(), size_t{0});
        QTRY_COMPARE(server->ConnectionCount(), 1);
    }

    // --- the bus ---------------------------------------------------------

    void EventsReachOtherClients() {
        const auto server = Start();
        QVERIFY(server);
        const auto listener = Connected();
        const auto publisher = Connected();
        QVERIFY(listener && publisher);
        QSignalSpy heard(listener.get(), &RpcClient::NotificationReceived);
        QSignalSpy echoed(publisher.get(), &RpcClient::NotificationReceived);

        QFuture<rpc::Result> subscribed = listener->Request(
            "bus.subscribe", QJsonObject{{"pattern", "test.*"}});
        QVERIFY(WaitFor(subscribed));
        const QJsonValue id =
            subscribed.result().value().toObject().value("subscription");
        QVERIFY(id.isDouble());

        QFuture<rpc::Result> published = publisher->Request(
            "bus.publish", QJsonObject{{"name", "test.ping"},
                                       {"payload", QJsonObject{{"n", 1}}}});
        QVERIFY(WaitFor(published));
        QCOMPARE(published.result().value(), QJsonValue());
        QTRY_COMPARE(heard.count(), 1);
        QCOMPARE(heard.at(0).at(0).toString(), QString("bus.event"));
        QCOMPARE(heard.at(0).at(1).value<QJsonValue>().toObject(),
                 (QJsonObject{
                     {"subscription", id},
                     {"name", "test.ping"},
                     {"payload", QJsonObject{{"n", 1}}},
                 }));

        // Not under the prefix: nothing.
        QFuture<rpc::Result> other = publisher->Request(
            "bus.publish", QJsonObject{{"name", "tests.ping"}});
        QVERIFY(WaitFor(other));
        QTest::qWait(20);
        QCOMPARE(heard.count(), 1);
        QCOMPARE(echoed.count(), 0);
    }

    void EventTooBigForAFrameIsRefused() {
        const auto server = Start();
        QVERIFY(server);
        const auto listener = Connected();
        const auto publisher = Connected();
        QVERIFY(listener && publisher);
        QSignalSpy heard(listener.get(), &RpcClient::NotificationReceived);
        QVERIFY(WaitFor(listener->Request("bus.subscribe",
                                          QJsonObject{{"pattern", "test.*"}})));

        // The request fits a frame, the event it makes does not: it is a few
        // bytes longer.
        const auto requestSize = [](const QString& payload) {
            return QJsonDocument(
                       QJsonObject{
                           {"jsonrpc", "2.0"},
                           {"id", 99999},
                           {"method", "bus.publish"},
                           {"params", QJsonObject{{"name", "test.big"},
                                                  {"payload", payload}}},
                       })
                .toJson(QJsonDocument::Compact)
                .size();
        };
        const qsizetype room =
            qsizetype{MAX_FRAME_LENGTH} - 1 - requestSize(QString());
        QFuture<rpc::Result> published = publisher->Request(
            "bus.publish", QJsonObject{{"name", "test.big"},
                                       {"payload", QString(room, u'x')}});
        QVERIFY(WaitFor(published));
        QCOMPARE(CodeOf(published.result()),
                 Code(rpc::EErrorCode::InvalidParams));

        // The subscriber heard nothing and is still there.
        QVERIFY(WaitFor(publisher->Request(
            "bus.publish", QJsonObject{{"name", "test.small"}})));
        QTRY_COMPARE(heard.count(), 1);
        QCOMPARE(heard.at(0).at(1).value<QJsonValue>().toObject().value("name"),
                 QJsonValue("test.small"));
        QCOMPARE(server->ConnectionCount(), 2);
    }

    void PublishWithoutPayloadSendsNull() {
        const auto server = Start();
        QVERIFY(server);
        const auto client = Connected();
        QVERIFY(client);
        QSignalSpy heard(client.get(), &RpcClient::NotificationReceived);
        QVERIFY(WaitFor(
            client->Request("bus.subscribe", QJsonObject{{"pattern", "*"}})));
        QVERIFY(WaitFor(
            client->Request("bus.publish", QJsonObject{{"name", "test.x"}})));
        QTRY_COMPARE(heard.count(), 1);
        const QJsonObject event =
            heard.at(0).at(1).value<QJsonValue>().toObject();
        QVERIFY(event.contains("payload"));
        QCOMPARE(event.value("payload"), QJsonValue());
    }

    void BadBusParamsAreInvalid_data() {
        QTest::addColumn<QString>("method");
        QTest::addColumn<QJsonValue>("params");
        QTest::newRow("subscribe, no params")
            << "bus.subscribe" << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("subscribe, bad pattern")
            << "bus.subscribe" << QJsonValue(QJsonObject{{"pattern", "a..b"}});
        QTest::newRow("subscribe, pattern not a string")
            << "bus.subscribe" << QJsonValue(QJsonObject{{"pattern", 1}});
        QTest::newRow("publish, bad name")
            << "bus.publish" << QJsonValue(QJsonObject{{"name", "a.*"}});
        QTest::newRow("publish, no name")
            << "bus.publish" << QJsonValue(QJsonObject{{"payload", 1}});
        QTest::newRow("unsubscribe, unknown id")
            << "bus.unsubscribe"
            << QJsonValue(QJsonObject{{"subscription", 999}});
        QTest::newRow("unsubscribe, negative id")
            << "bus.unsubscribe"
            << QJsonValue(QJsonObject{{"subscription", -1}});
        QTest::newRow("unsubscribe, id as string")
            << "bus.unsubscribe"
            << QJsonValue(QJsonObject{{"subscription", "1"}});
    }

    void BadBusParamsAreInvalid() {
        QFETCH(QString, method);
        QFETCH(QJsonValue, params);
        const auto server = Start();
        QVERIFY(server);
        const auto client = Connected();
        QVERIFY(client);
        QFuture<rpc::Result> answer = client->Request(method, params);
        QVERIFY(WaitFor(answer));
        QCOMPARE(CodeOf(answer.result()), Code(rpc::EErrorCode::InvalidParams));
        QCOMPARE(server->Bus().SubscriptionCount(), size_t{0});
    }

    void UnsubscribeStopsEvents() {
        const auto server = Start();
        QVERIFY(server);
        const auto client = Connected();
        QVERIFY(client);
        QSignalSpy heard(client.get(), &RpcClient::NotificationReceived);
        QFuture<rpc::Result> subscribed = client->Request(
            "bus.subscribe", QJsonObject{{"pattern", "test.x"}});
        QVERIFY(WaitFor(subscribed));
        const QJsonValue id =
            subscribed.result().value().toObject().value("subscription");

        QFuture<rpc::Result> removed = client->Request(
            "bus.unsubscribe", QJsonObject{{"subscription", id}});
        QVERIFY(WaitFor(removed));
        QVERIFY(removed.result());
        QCOMPARE(server->Bus().SubscriptionCount(), size_t{0});

        QVERIFY(WaitFor(
            client->Request("bus.publish", QJsonObject{{"name", "test.x"}})));
        QTest::qWait(20);
        QCOMPARE(heard.count(), 0);

        // Gone means gone.
        QFuture<rpc::Result> again = client->Request(
            "bus.unsubscribe", QJsonObject{{"subscription", id}});
        QVERIFY(WaitFor(again));
        QCOMPARE(CodeOf(again.result()), Code(rpc::EErrorCode::InvalidParams));
    }

    void OnlyTheOwnerUnsubscribes() {
        const auto server = Start();
        QVERIFY(server);
        const auto owner = Connected();
        const auto stranger = Connected();
        QVERIFY(owner && stranger);
        QSignalSpy heard(owner.get(), &RpcClient::NotificationReceived);
        QFuture<rpc::Result> subscribed =
            owner->Request("bus.subscribe", QJsonObject{{"pattern", "test.x"}});
        QVERIFY(WaitFor(subscribed));
        const QJsonValue id =
            subscribed.result().value().toObject().value("subscription");

        QFuture<rpc::Result> taken = stranger->Request(
            "bus.unsubscribe", QJsonObject{{"subscription", id}});
        QVERIFY(WaitFor(taken));
        QCOMPARE(CodeOf(taken.result()), Code(rpc::EErrorCode::InvalidParams));

        QVERIFY(WaitFor(
            stranger->Request("bus.publish", QJsonObject{{"name", "test.x"}})));
        QTRY_COMPARE(heard.count(), 1);
    }

    void SubscriptionsEndWithTheConnection() {
        const auto server = Start();
        QVERIFY(server);
        auto client = Connected();
        QVERIFY(client);
        for (const char* pattern : {"test.*", "*", "test.x"}) {
            QVERIFY(WaitFor(client->Request(
                "bus.subscribe", QJsonObject{{"pattern", pattern}})));
        }
        QCOMPARE(server->Bus().SubscriptionCount(), size_t{3});
        client.reset();
        QTRY_COMPARE(server->ConnectionCount(), 0);
        QCOMPARE(server->Bus().SubscriptionCount(), size_t{0});
        // Publishing into the void is fine.
        QVERIFY(server->Bus().Publish("test.x", 1));
    }

    void DaemonEventsReachClients() {
        const auto server = Start();
        QVERIFY(server);
        const auto client = Connected();
        QVERIFY(client);
        QSignalSpy heard(client.get(), &RpcClient::NotificationReceived);
        QVERIFY(WaitFor(client->Request("bus.subscribe",
                                        QJsonObject{{"pattern", "plugin.*"}})));
        QVERIFY(server->Bus().Publish("plugin.backendCrashed", "fixture"));
        QTRY_COMPARE(heard.count(), 1);
        QCOMPARE(
            heard.at(0).at(1).value<QJsonValue>().toObject().value("payload"),
            QJsonValue("fixture"));
    }

    // --- the client ------------------------------------------------------

    void ConnectingToNothingFails() {
        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(SocketPath());
        QVERIFY(WaitFor(hello));
        QCOMPARE(CodeOf(hello.result()),
                 Code(protocol::EErrorCode::ConnectionLost));
        QVERIFY(!client.IsConnected());
        QFuture<rpc::Result> later = client.Request("test.echo");
        QVERIFY(later.isFinished());
        QCOMPARE(CodeOf(later.result()),
                 Code(protocol::EErrorCode::ConnectionLost));
    }

    void RequestBeforeConnectFails() {
        RpcClient client;
        QFuture<rpc::Result> early = client.Request("test.echo");
        QVERIFY(early.isFinished());
        QCOMPARE(CodeOf(early.result()),
                 Code(protocol::EErrorCode::ConnectionLost));
    }

    void ConnectingTwiceFails() {
        const auto server = Start();
        QVERIFY(server);
        const auto client = Connected();
        QVERIFY(client);
        QFuture<rpc::Result> again = client->Connect(SocketPath());
        QVERIFY(again.isFinished());
        QVERIFY(!again.result());
        QVERIFY(client->IsConnected());
    }

    void RequestsWhileConnectingAreSent() {
        const auto server = Start();
        QVERIFY(server);
        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(SocketPath());
        QFuture<rpc::Result> echo = client.Request("test.echo", QJsonArray{3});
        QVERIFY(WaitFor(echo));
        QVERIFY(hello.isFinished());
        QCOMPARE(echo.result().value(), QJsonValue(QJsonArray{3}));
    }

    void DestroyedClientFailsItsRequests() {
        const auto server = Start();
        QVERIFY(server);
        auto client = Connected();
        QVERIFY(client);
        QFuture<rpc::Result> slow = client->Request("test.wait");
        client.reset();
        QVERIFY(slow.isFinished());
        QCOMPARE(CodeOf(slow.result()),
                 Code(protocol::EErrorCode::ConnectionLost));
    }

    // --- the socket ------------------------------------------------------

    void SocketDirectoryIsCreatedPrivate() {
        const QString directory = Dir_->filePath("run");
        Server server;
        QVERIFY(server.Listen(directory + "/roostd.sock"));
        struct stat info{};
        QCOMPARE(::lstat(QFile::encodeName(directory).constData(), &info), 0);
        QCOMPARE(info.st_mode & ACCESSPERMS, mode_t{S_IRWXU});
    }

    void OpenDirectoryIsRefused() {
        const QString directory = Dir_->filePath("open");
        QVERIFY(QDir().mkdir(directory));
        QVERIFY(QFile::setPermissions(
            directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                           QFile::ReadGroup | QFile::ExeGroup));
        Server server;
        const auto listening = server.Listen(directory + "/roostd.sock");
        QVERIFY(!listening);
        QVERIFY2(listening.error().contains("open to other users"),
                 qPrintable(listening.error()));
        QVERIFY(!QFile::exists(directory + "/roostd.sock"));
    }

    void SymlinkedDirectoryIsRefused() {
        const QString real = Dir_->filePath("real");
        const QString link = Dir_->filePath("link");
        QVERIFY(QDir().mkdir(real));
        QVERIFY(QFile::link(real, link));
        Server server;
        const auto listening = server.Listen(link + "/roostd.sock");
        QVERIFY(!listening);
        QVERIFY2(listening.error().contains("not a directory"),
                 qPrintable(listening.error()));
    }

    void MissingParentIsAnError() {
        Server server;
        const auto listening =
            server.Listen(Dir_->filePath("no/such/roostd.sock"));
        QVERIFY(!listening);
        QVERIFY2(listening.error().contains("cannot create"),
                 qPrintable(listening.error()));
    }

    void BadPathsAreRefused() {
        Server server;
        QVERIFY(!server.Listen("roostd.sock"));
        const auto tooLong = server.Listen(Dir_->filePath(QString(200, u'a')));
        QVERIFY(!tooLong);
        QVERIFY2(tooLong.error().contains("longer than"),
                 qPrintable(tooLong.error()));
    }

    void RefusedHelloDropsTheConnection() {
        // A roostd of another build, as the client sees it.
        QLocalServer fake;
        QVERIFY(fake.listen(SocketPath()));
        QLocalSocket* accepted = nullptr;
        FrameDecoder decoder;
        connect(&fake, &QLocalServer::newConnection, [&] {
            accepted = fake.nextPendingConnection();
            connect(accepted, &QLocalSocket::readyRead, [&] {
                decoder.Feed(accepted->readAll());
                const std::optional<Frame> frame = decoder.Next();
                if (!frame) {
                    return;
                }
                const QJsonValue id =
                    QJsonDocument::fromJson(frame->Body).object().value("id");
                accepted->write(
                    EncodeFrame(
                        EFrameKind::Rpc,
                        QJsonDocument(
                            QJsonObject{
                                {"jsonrpc", "2.0"},
                                {"id", id},
                                {"error", QJsonObject{{"code", -32001},
                                                      {"message", "too old"}}},
                            })
                            .toJson(QJsonDocument::Compact))
                        .value());
            });
        });

        RpcClient client;
        QSignalSpy disconnected(&client, &RpcClient::Disconnected);
        QFuture<rpc::Result> hello = client.Connect(SocketPath());
        QFuture<rpc::Result> queued = client.Request("test.echo");
        QVERIFY(WaitFor(hello));
        QCOMPARE(CodeOf(hello.result()),
                 Code(protocol::EErrorCode::VersionMismatch));
        QVERIFY(queued.isFinished());
        QCOMPARE(CodeOf(queued.result()),
                 Code(protocol::EErrorCode::ConnectionLost));
        QVERIFY(!client.IsConnected());
        QCOMPARE(disconnected.count(), 1);
        QCOMPARE(disconnected.at(0).at(0).toString(),
                 QString("roostd refused hello: too old"));
    }

    void SecondServerIsRefused() {
        const auto first = Start();
        QVERIFY(first);
        Server second;
        const auto listening = second.Listen(SocketPath());
        QVERIFY(!listening);
        QVERIFY2(listening.error().contains("another roostd"),
                 qPrintable(listening.error()));
        // The first one still serves.
        QVERIFY(Connected());
        QVERIFY(!first->Listen(SocketPath()));
    }

    void SocketOfADeadDaemonIsReplaced() {
        // A socket bound and never unlinked, as a killed roostd leaves it.
        const QByteArray path = QFile::encodeName(SocketPath());
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            QFAIL("no socket");
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.constData(),
                    static_cast<size_t>(path.size()));
        QCOMPARE(::bind(fd, reinterpret_cast<const sockaddr*>(&address),
                        sizeof(address)),
                 0);
        ::close(fd);
        QVERIFY(QFile::exists(SocketPath()));

        const auto server = Start();
        QVERIFY(server);
        QVERIFY(Connected());
    }

    void LockOfADeadDaemonIsTakenOver_data() {
        QTest::addColumn<QString>("host");
        QTest::newRow("same host") << QSysInfo::machineHostName();
        // A Mac renames itself with the network it joins.
        QTest::newRow("renamed host") << "some-old-hostname.local";
    }

    void LockOfADeadDaemonIsTakenOver() {
        QFETCH(QString, host);
        QProcess gone;
        gone.start("/usr/bin/true", {});
        QVERIFY(gone.waitForStarted(TIMEOUT_MS));
        const qint64 pid = gone.processId();
        QVERIFY(gone.waitForFinished(TIMEOUT_MS));

        // As QLockFile writes it.
        QFile lock(SocketPath() + ".lock");
        QVERIFY(lock.open(QIODevice::WriteOnly));
        lock.write(QByteArray::number(pid) + "\nroostd\n" + host.toUtf8() +
                   '\n' + QSysInfo::machineUniqueId() + '\n' +
                   QSysInfo::bootUniqueId() + '\n');
        lock.close();

        const auto server = Start();
        QVERIFY(server);
        QVERIFY(Connected());
    }

    void SocketGoesWithTheServer() {
        auto server = Start();
        QVERIFY(server);
        server.reset();
        QVERIFY(!QFile::exists(SocketPath()));
        // And the lock with it: the next one starts.
        QVERIFY(Start());
    }

    void PeerUidIsOurs() {
        int fds[2] = {-1, -1};
        QCOMPARE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
        const std::optional<uid_t> uid = PeerUid(fds[0]);
        ::close(fds[0]);
        ::close(fds[1]);
        QVERIFY(uid);
        QCOMPARE(*uid, ::geteuid());
        QVERIFY(!PeerUid(-1));
    }
};

QTEST_GUILESS_MAIN(ServerTest)
#include "server_test.moc"
