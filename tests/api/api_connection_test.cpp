// The connection of the window's API: Connected, Disconnected and Refused, and
// the two additions to the IO thread that the API stands on, Request and
// Notified.

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "api/api.hpp"
#include "api_test_support.hpp"
#include "core/frame.hpp"
#include "core/protocol.hpp"
#include "helpers/refusing_server.hpp"
#include "helpers/roostd_process.hpp"

using namespace roost;
using namespace roost::api_test;

class ApiConnectionTest : public QObject {
    Q_OBJECT

private slots:
    void ConnectsAndSaysSo() {
        roost::testing::RoostdProcess daemon;
        QVERIFY(daemon.Start(ROOSTD_PATH));
        api::Api api;
        QVERIFY(!api.IsConnected());
        QSignalSpy connected(&api, &api::Api::Connected);
        QSignalSpy refused(&api, &api::Api::Refused);
        api.Connect(daemon.SocketPath());
        // The signal may be said inside Connect: look at the spy before
        // waiting.
        QVERIFY(!connected.isEmpty() || connected.wait(TIMEOUT_MS));
        QVERIFY(api.IsConnected());
        QVERIFY(refused.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(api.Plugins().IsLoaded(), TIMEOUT_MS);
    }

    void NoDaemonIsDisconnectedNotRefused() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        api::Api api;
        QSignalSpy disconnected(&api, &api::Api::Disconnected);
        QSignalSpy refused(&api, &api::Api::Refused);
        QSignalSpy connected(&api, &api::Api::Connected);
        api.Connect(dir.filePath("none.sock"));
        QVERIFY(disconnected.wait(TIMEOUT_MS));
        QVERIFY(refused.isEmpty());
        QVERIFY(connected.isEmpty());
        QVERIFY(!api.IsConnected());
    }

    void ADaemonOfAnotherBuildRefuses() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        roost::testing::RefusingServer daemon;
        QVERIFY(daemon.Listen(dir.filePath("old.sock")));
        api::Api api;
        QSignalSpy refused(&api, &api::Api::Refused);
        QSignalSpy disconnected(&api, &api::Api::Disconnected);
        QSignalSpy connected(&api, &api::Api::Connected);
        api.Connect(dir.filePath("old.sock"));
        QVERIFY(!refused.isEmpty() || refused.wait(TIMEOUT_MS));
        const auto error = qvariant_cast<rpc::Error>(refused.at(0).at(0));
        QCOMPARE(error.Code,
                 static_cast<int>(protocol::EErrorCode::VersionMismatch));
        QCOMPARE(error.Data.toObject().value("ghosttyVt").toString(),
                 QString(roost::testing::RefusingServer::GHOSTTY_VT));
        QCOMPARE(daemon.Hellos(), 1);
        QVERIFY(connected.isEmpty());
        QVERIFY(!api.IsConnected());
    }

    void CallsBeforeTheConnectionFail() {
        api::Api api;
        const auto answer =
            api.Dispatch().Call("sessions.list", QJsonValue(), &api);
        QVERIFY(WaitFor(answer));
        QVERIFY(!answer.result());
        QCOMPARE(answer.result().error().Code,
                 static_cast<int>(protocol::EErrorCode::ConnectionLost));
    }

    void TheIoThreadRequestsAndNotifies() {
        roost::testing::RoostdProcess daemon;
        QVERIFY(daemon.Start(ROOSTD_PATH));
        api::Api api;
        QSignalSpy connected(&api, &api::Api::Connected);
        api.Connect(daemon.SocketPath());
        QVERIFY(!connected.isEmpty() || connected.wait(TIMEOUT_MS));
        QSignalSpy notified(&api.Io(), &IoThread::Notified);

        // Any method of the protocol, and its error as it came.
        const auto unknown = api.Io().Request("no.such.method");
        QVERIFY(WaitFor(unknown));
        QCOMPARE(unknown.result().error().Code,
                 static_cast<int>(rpc::EErrorCode::MethodNotFound));

        // An event of the bus comes as a notification, not as an answer.
        const auto subscribed =
            api.Io().Request("bus.subscribe", QJsonObject{{"pattern", "t.*"}});
        QVERIFY(WaitFor(subscribed));
        QVERIFY(subscribed.result());
        const auto published = api.Io().Request(
            "bus.publish", QJsonObject{{"name", "t.a"}, {"payload", 5}});
        QVERIFY(WaitFor(published));
        QTRY_VERIFY_WITH_TIMEOUT(!notified.isEmpty(), TIMEOUT_MS);
        QCOMPARE(notified.at(0).at(0).toString(), QString("bus.event"));
        QCOMPARE(notified.at(0).at(1).toJsonValue().toObject().value("name"),
                 QJsonValue("t.a"));
    }
};

QTEST_GUILESS_MAIN(ApiConnectionTest)
#include "api_connection_test.moc"
