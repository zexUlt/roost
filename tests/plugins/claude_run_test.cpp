// ClaudeRun and StopRun on a run that never got a process: the working
// directory goes away or cannot be entered between the check and the start,
// chdir fails in the child, and QProcess reports FailedToStart with pid 0.
// Signalling the group 0 there would kill the adapter's own group, so the run
// itself is done in a child process of this binary, in a session of its own:
// a regression kills the child, never the test runner.

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <cstdio>
#include <cstring>

#include "claude/backend/claude_run.hpp"

using namespace roost::claude;

namespace {

constexpr int TIMEOUT_MS = 10000;
constexpr const char* PROBE = "--probe-start-failure";

// The probe: one run in the directory, which must fail to start, then the
// process is still alive to say so. Exit 0 and the reason on stdout.
int Probe(const QString& directory) {
    auto* run = new ClaudeRun({.Program = QStringLiteral("/usr/bin/true"),
                               .Args = {QStringLiteral("x")},
                               .WorkingDirectory = directory,
                               .Timeout = std::chrono::milliseconds(5000)});
    QFuture<RunResult> future = run->Start();
    QEventLoop loop;
    QFutureWatcher<RunResult> watcher;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop,
                     &QEventLoop::quit);
    QTimer::singleShot(TIMEOUT_MS, &loop, &QEventLoop::quit);
    watcher.setFuture(future);
    loop.exec();
    if (!future.isFinished() || future.result().has_value()) {
        return 2;
    }
    std::puts(qPrintable(future.result().error()));
    return 0;
}

}  // namespace

class ClaudeRunTest : public QObject {
    Q_OBJECT

private:
    // Runs the probe in a session of its own and checks that it lived.
    void VerifyProbeSurvives(const QString& directory) {
        QProcess probe;
        probe.setProcessChannelMode(QProcess::SeparateChannels);
        probe.setUnixProcessParameters(
            QProcess::UnixProcessFlag::CreateNewSession);
        probe.start(QCoreApplication::applicationFilePath(),
                    {QString::fromLatin1(PROBE), directory});
        QVERIFY(probe.waitForStarted(TIMEOUT_MS));
        QVERIFY(probe.waitForFinished(TIMEOUT_MS * 2));
        QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
        QCOMPARE(probe.exitCode(), 0);
        QVERIFY2(probe.readAllStandardOutput().startsWith("cannot run claude"),
                 "the reason of the failed start");
    }

private slots:
    void ADirectoryThatIsGoneFailsTheRunAndSignalsNoGroup() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        VerifyProbeSurvives(dir.filePath("gone"));
    }

    void ADirectoryThatCannotBeEnteredFailsTheRunAndSignalsNoGroup() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString locked = dir.filePath("locked");
        QVERIFY(QDir().mkpath(locked));
        const auto restore = qScopeGuard([&] {
            QFile::setPermissions(
                locked, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        });
        QVERIFY(QFile::setPermissions(locked, QFileDevice::Permissions()));
        if (QFileInfo(locked).isExecutable()) {
            QSKIP("a mode 000 directory can still be entered (root)");
        }
        VerifyProbeSurvives(locked);
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc == 3 && std::strcmp(argv[1], PROBE) == 0) {
        return Probe(QString::fromLocal8Bit(argv[2]));
    }
    ClaudeRunTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "claude_run_test.moc"
