#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "core/rpc_client.hpp"

using namespace roost;

namespace {

constexpr int TIMEOUT_MS = 10000;

bool Greets(const QString& socketPath) {
    const QDeadlineTimer deadline(TIMEOUT_MS);
    while (!deadline.hasExpired()) {
        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(socketPath);
        if (QTest::qWaitFor([&] { return hello.isFinished(); }, TIMEOUT_MS) &&
            hello.result()) {
            return true;
        }
        QTest::qWait(20);
    }
    return false;
}

rpc::Result Call(RpcClient& client, const QString& method,
                 const QJsonObject& params) {
    QFuture<rpc::Result> future = client.Request(method, params);
    if (!QTest::qWaitFor([&] { return future.isFinished(); }, TIMEOUT_MS)) {
        return std::unexpected(rpc::Error{.Message = "timed out"});
    }
    return future.result();
}

}  // namespace

// The state of a plugin outlives roostd: the real binary, stopped and
// started again over the same ROOST_CONFIG.
class RoostdStoreTest : public QObject {
    Q_OBJECT

private:
    QString SocketPath() const { return Dir_->filePath("roostd.sock"); }

    std::unique_ptr<QProcess> Launch() const {
        auto process = std::make_unique<QProcess>();
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("ROOST_SOCKET", SocketPath());
        environment.insert("ROOST_CONFIG", Dir_->filePath("config"));
        environment.insert("ROOST_PLUGINS_PATH", Dir_->filePath("plugins"));
        process->setProcessEnvironment(environment);
        process->start(ROOSTD_PATH, {});
        return process;
    }

    std::unique_ptr<QTemporaryDir> Dir_;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
    }

    void DocumentSurvivesARestart() {
        const QDir plugin(Dir_->filePath("plugins/keeper"));
        QVERIFY(plugin.mkpath("."));
        QFile manifest(plugin.filePath("plugin.json"));
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        QVERIFY(manifest.write(R"({"id": "keeper", "version": "1",
                                   "api": "0"})") > 0);
        manifest.close();
        const QJsonObject document{{"pins", QJsonArray{"a", "b"}}, {"n", 3}};

        {
            const auto daemon = Launch();
            QVERIFY(daemon->waitForStarted(TIMEOUT_MS));
            QVERIFY(Greets(SocketPath()));
            RpcClient client;
            QFuture<rpc::Result> hello = client.Connect(SocketPath());
            QVERIFY(QTest::qWaitFor([&] { return hello.isFinished(); },
                                    TIMEOUT_MS));
            QVERIFY(hello.result());
            QVERIFY(Call(client, "store.set",
                         {{"plugin", "keeper"}, {"document", document}}));
            daemon->terminate();
            QVERIFY(daemon->waitForFinished(TIMEOUT_MS));
            QCOMPARE(daemon->exitCode(), 0);
        }

        const auto daemon = Launch();
        QVERIFY(daemon->waitForStarted(TIMEOUT_MS));
        QVERIFY(Greets(SocketPath()));
        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(SocketPath());
        QVERIFY(
            QTest::qWaitFor([&] { return hello.isFinished(); }, TIMEOUT_MS));
        QVERIFY(hello.result());
        const rpc::Result got =
            Call(client, "store.get", {{"plugin", "keeper"}});
        QVERIFY2(got, qPrintable(got ? QString() : got.error().Message));
        QCOMPARE(*got, QJsonValue(QJsonObject{{"document", document}}));
        daemon->terminate();
        QVERIFY(daemon->waitForFinished(TIMEOUT_MS));
    }
};

QTEST_GUILESS_MAIN(RoostdStoreTest)
#include "roostd_store_test.moc"
