#include <QDeadlineTimer>
#include <QFile>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "core/protocol.hpp"
#include "core/rpc_client.hpp"
#include "core/version.hpp"

using namespace roost;

namespace {

constexpr int TIMEOUT_MS = 10000;

// Whether a roostd, perhaps still starting, answers hello at the path.
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

}  // namespace

// roostd as it runs: a process that finds its socket through the environment.
class RoostdTest : public QObject {
    Q_OBJECT

private:
    QString SocketPath() const { return Dir_->filePath("roostd.sock"); }

    std::unique_ptr<QProcess> Launch() const {
        auto process = std::make_unique<QProcess>();
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("ROOST_SOCKET", SocketPath());
        environment.insert("ROOST_CONFIG", Dir_->filePath("config"));
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

    void ServesItsSocket() {
        const auto daemon = Launch();
        QVERIFY(daemon->waitForStarted(TIMEOUT_MS));
        // The socket appears once it listens: Qt binds it aside and moves it
        // into place.
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(SocketPath()), TIMEOUT_MS);

        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(SocketPath());
        QVERIFY(
            QTest::qWaitFor([&] { return hello.isFinished(); }, TIMEOUT_MS));
        QVERIFY(hello.result());
        QCOMPARE(hello.result()->toObject().value("roostd").toString(),
                 QString::fromStdString(VersionString()));

        daemon->terminate();
        QVERIFY(daemon->waitForFinished(TIMEOUT_MS));
    }

    void SecondDaemonExits() {
        const auto first = Launch();
        QVERIFY(first->waitForStarted(TIMEOUT_MS));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(SocketPath()), TIMEOUT_MS);

        const auto second = Launch();
        QVERIFY(second->waitForFinished(TIMEOUT_MS));
        QCOMPARE(second->exitStatus(), QProcess::NormalExit);
        QCOMPARE(second->exitCode(), 1);
        const QString error = QString::fromUtf8(second->readAllStandardError());
        QVERIFY2(error.contains("another roostd"), qPrintable(error));

        first->terminate();
        QVERIFY(first->waitForFinished(TIMEOUT_MS));
    }

    void KilledDaemonIsReplaced() {
        const auto killed = Launch();
        QVERIFY(killed->waitForStarted(TIMEOUT_MS));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(SocketPath()), TIMEOUT_MS);
        killed->kill();
        QVERIFY(killed->waitForFinished(TIMEOUT_MS));
        // Its socket and lock are still there.
        QVERIFY(QFile::exists(SocketPath()));

        const auto next = Launch();
        QVERIFY(next->waitForStarted(TIMEOUT_MS));
        QVERIFY(Greets(SocketPath()));
        QCOMPARE(next->state(), QProcess::Running);
        next->terminate();
        QVERIFY(next->waitForFinished(TIMEOUT_MS));
    }
};

QTEST_GUILESS_MAIN(RoostdTest)
#include "roostd_test.moc"
