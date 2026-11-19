// The pid of the process at the other end of the connection (RpcClient::
// PeerPid): the window signals only the roostd that refused it. Here the
// server is in this process, so the peer is this process.

#include <QCoreApplication>
#include <QJsonObject>
#include <QLocalServer>
#include <QTemporaryDir>
#include <QTest>

#include "core/protocol.hpp"
#include "core/rpc_client.hpp"
#include "helpers/refusing_server.hpp"

using namespace roost;

class RpcClientPeerPidTest : public QObject {
    Q_OBJECT

private slots:
    void NoPeerBeforeTheConnection() {
        RpcClient client;
        QCOMPARE(client.PeerPid(), qint64(0));
    }

    void ThePeerOfALocalServerIsThisProcess() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QLocalServer server;
        QVERIFY(server.listen(dir.filePath("peer.sock")));
        RpcClient client;
        auto hello = client.Connect(dir.filePath("peer.sock"));
        QTRY_VERIFY(client.IsConnected());
        QCOMPARE(client.PeerPid(), QCoreApplication::applicationPid());
    }

    // The refusal carries the peer in its data, next to what the server sent.
    void ARefusedHelloCarriesThePeerPid() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        testing::RefusingServer server;
        QVERIFY(server.Listen(dir.filePath("peer.sock")));
        RpcClient client;
        auto hello = client.Connect(dir.filePath("peer.sock"));
        QTRY_VERIFY(hello.isFinished());
        const auto result = hello.result();
        QVERIFY(!result);
        const QJsonObject data = result.error().Data.toObject();
        QCOMPARE(data.value(QLatin1String(protocol::PEER_PID_KEY)).toInteger(),
                 QCoreApplication::applicationPid());
        QCOMPARE(data.value(QStringLiteral("ghosttyVt")).toString(),
                 QString(testing::RefusingServer::GHOSTTY_VT));
    }
};

QTEST_MAIN(RpcClientPeerPidTest)
#include "rpc_client_peer_pid_test.moc"
