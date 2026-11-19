// DaemonLauncher with a real Api and the real roostd binary: it starts roostd
// when an attempt finds nobody at the socket (no file, or a stale one), and
// never when a server answers, a refusing one included. The roostd it starts
// is detached into a session of its own, so every case ends by stopping it by
// the pid in its lock file (DetachedDaemons).

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "api/api.hpp"
#include "helpers/detached_daemons.hpp"
#include "helpers/refusing_server.hpp"
#include "helpers/roostd_process.hpp"
#include "helpers/scoped_env.hpp"
#include "roost/daemon_launcher.hpp"

using namespace roost;
using namespace std::chrono_literals;

namespace {

constexpr int TIMEOUT_MS = 20000;

api::ApiOptions FastRetry() {
    return {.FirstReconnectDelay = 20ms, .MaxReconnectDelay = 40ms};
}

// How many roostd processes run with the log of this directory on their
// command line (the launcher passes `--log <dir>/run/roostd.log`).
int CountDaemons(const QString& dir) {
    QProcess pgrep;
    pgrep.start("pgrep", {"-f", dir + "/run/roostd.log"});
    pgrep.waitForFinished(TIMEOUT_MS);
    int count = 0;
    for (const QByteArray& line : pgrep.readAllStandardOutput().split('\n')) {
        count += line.trimmed().isEmpty() ? 0 : 1;
    }
    return count;
}

}  // namespace

class DaemonLauncherTest : public QObject {
    Q_OBJECT

private:
    QString Socket() const { return Dir_->filePath("run/roostd.sock"); }
    QString Lock() const { return Socket() + ".lock"; }

    DaemonLauncher::Command Command() const {
        return {QStringLiteral(ROOSTD_PATH),
                {"--log", Dir_->filePath("run/roostd.log")},
                Dir_->filePath("run/roostd.start.log")};
    }

    // Counts the starts, and starts for real unless `spawn` is off.
    DaemonLauncher::StartFunction Counting(bool spawn = true) {
        return [this, spawn](const DaemonLauncher::Command& command,
                             QString& error) {
            ++Starts_;
            return !spawn || DaemonLauncher::StartDetached(command, error);
        };
    }

    QTemporaryDir* Dir_ = nullptr;
    std::unique_ptr<QTemporaryDir> Owned_;
    std::unique_ptr<testing::DetachedDaemons> Daemons_;
    std::vector<std::unique_ptr<testing::ScopedEnv>> Env_;
    std::atomic<int> Starts_ = 0;

private slots:
    void init() {
        Owned_ = std::make_unique<QTemporaryDir>();
        Dir_ = Owned_.get();
        QVERIFY(Dir_->isValid());
        QVERIFY(QDir(Dir_->path()).mkpath("run"));
        QVERIFY(QDir(Dir_->path()).mkpath("plugins"));
        QVERIFY(QDir(Dir_->path()).mkpath("home"));
        QFile::setPermissions(Dir_->filePath("run"), QFile::ReadOwner |
                                                         QFile::WriteOwner |
                                                         QFile::ExeOwner);
        // What the started roostd (and the window) read, as in the window.
        Env_.clear();
        for (const auto& [name, value] :
             {std::pair{"ROOST_SOCKET", Socket()},
              std::pair{"ROOST_CONFIG", Dir_->filePath("config")},
              std::pair{"ROOST_PLUGINS_PATH", Dir_->filePath("plugins")},
              std::pair{"HOME", Dir_->filePath("home")}}) {
            Env_.push_back(
                std::make_unique<testing::ScopedEnv>(name, value.toUtf8()));
        }
        Starts_ = 0;
        Daemons_ =
            std::make_unique<testing::DetachedDaemons>(QStringList{Lock()});
    }

    void cleanup() {
        Daemons_.reset();
        Env_.clear();
        // Nothing of this case is left running. A roostd that dies early may
        // still be exiting when the case ends: it has to be gone in time.
        QTRY_COMPARE_WITH_TIMEOUT(CountDaemons(Dir_->path()), 0, TIMEOUT_MS);
        Owned_.reset();
    }

    void StartsRoostdWhenThereIsNoSocket() {
        api::Api api(FastRetry());
        DaemonLauncher launcher(
            api, Command(), DaemonLauncher::DEFAULT_START_TIMEOUT, Counting());
        QSignalSpy connected(&api, &api::Api::Connected);
        QSignalSpy starting(&launcher, &DaemonLauncher::StartingChanged);
        QVERIFY(!QFile::exists(Socket()));
        api.Connect(Socket());
        QVERIFY(connected.wait(TIMEOUT_MS));
        QCOMPARE(Starts_.load(), 1);
        QVERIFY(!launcher.Starting());
        // True, then false once connected.
        QCOMPARE(starting.size(), 2);
        QVERIFY(starting.at(0).at(0).toBool());
        const qint64 pid = testing::DetachedDaemons::PidOf(Lock());
        QVERIFY(testing::DetachedDaemons::IsAlive(pid));
        QCOMPARE(CountDaemons(Dir_->path()), 1);
        QTRY_VERIFY_WITH_TIMEOUT(
            QFile::exists(Dir_->filePath("run/roostd.log")), TIMEOUT_MS);
    }

    void StartsRoostdOverAStaleSocketFile() {
        // A socket file that nobody listens on: bind, close, leave the file.
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            QFAIL("no socket");
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const QByteArray path = QFile::encodeName(Socket());
        QVERIFY(static_cast<size_t>(path.size()) < sizeof(address.sun_path));
        std::memcpy(address.sun_path, path.constData(),
                    static_cast<size_t>(path.size()));
        QCOMPARE(
            ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)),
            0);
        ::close(fd);
        QVERIFY(QFile::exists(Socket()));

        api::Api api(FastRetry());
        DaemonLauncher launcher(
            api, Command(), DaemonLauncher::DEFAULT_START_TIMEOUT, Counting());
        QSignalSpy noListener(&api, &api::Api::NoListener);
        QSignalSpy connected(&api, &api::Api::Connected);
        api.Connect(Socket());
        QVERIFY(connected.wait(TIMEOUT_MS));
        QVERIFY(!noListener.isEmpty());
        QCOMPARE(Starts_.load(), 1);
        QVERIFY(testing::DetachedDaemons::IsAlive(
            testing::DetachedDaemons::PidOf(Lock())));
    }

    void DoesNotStartWhenARoostdAnswers() {
        testing::RoostdProcess daemon;
        QVERIFY(daemon.Start(ROOSTD_PATH));
        api::Api api(FastRetry());
        DaemonLauncher launcher(
            api, Command(), DaemonLauncher::DEFAULT_START_TIMEOUT, Counting());
        QSignalSpy connected(&api, &api::Api::Connected);
        QSignalSpy noListener(&api, &api::Api::NoListener);
        api.Connect(daemon.SocketPath());
        QVERIFY(connected.wait(TIMEOUT_MS));
        QTest::qWait(300);
        QCOMPARE(Starts_.load(), 0);
        QVERIFY(noListener.isEmpty());
    }

    void DoesNotStartForAServerThatRefuses() {
        testing::RefusingServer server;
        QVERIFY(server.Listen(Socket()));
        api::Api api(FastRetry());
        DaemonLauncher launcher(
            api, Command(), DaemonLauncher::DEFAULT_START_TIMEOUT, Counting());
        QSignalSpy refused(&api, &api::Api::Refused);
        QSignalSpy noListener(&api, &api::Api::NoListener);
        api.Connect(Socket());
        QVERIFY(refused.wait(TIMEOUT_MS));
        QTest::qWait(300);
        QCOMPARE(Starts_.load(), 0);
        QVERIFY(noListener.isEmpty());
        QVERIFY(!launcher.Starting());
    }

    // The Api reports NoListener on every retry while the new roostd comes up;
    // only the first starts a process. Once the attempt is over (here: it
    // never came up) the next report starts again. The log of the starts and
    // of the ends of the attempts must alternate: no start inside an attempt.
    void StartsOncePerAttempt() {
        api::Api api(FastRetry());
        DaemonLauncher launcher(api, Command(), 1s, Counting(false));
        QStringList log;
        connect(
            &launcher, &DaemonLauncher::StartingChanged, this,
            [&log](bool starting) { log.append(starting ? "begin" : "end"); });
        QSignalSpy noListener(&api, &api::Api::NoListener);
        api.Connect(Socket());
        QTRY_VERIFY_WITH_TIMEOUT(noListener.size() >= 10, TIMEOUT_MS);
        QVERIFY(launcher.Starting() || log.size() >= 2);
        // Many reports, and the starts are one per attempt.
        QTRY_VERIFY_WITH_TIMEOUT(log.size() >= 4, TIMEOUT_MS);
        const int starts = Starts_.load();
        QVERIFY(starts >= 2);
        QVERIFY(noListener.size() > starts * 5);
        for (int i = 0; i < log.size(); ++i) {
            QCOMPARE(log.at(i), i % 2 == 0 ? QString("begin") : QString("end"));
        }
        // One start per begin (the one in flight may not have logged yet).
        QVERIFY(starts - (log.size() + 1) / 2 <= 0);
    }

    // Two windows at once: each starts a roostd, the socket lock leaves one,
    // and both connect to it.
    void TwoWindowsLeaveOneRoostd() {
        api::Api first(FastRetry());
        api::Api second(FastRetry());
        DaemonLauncher firstLauncher(first, Command(),
                                     DaemonLauncher::DEFAULT_START_TIMEOUT,
                                     Counting());
        DaemonLauncher secondLauncher(second, Command(),
                                      DaemonLauncher::DEFAULT_START_TIMEOUT,
                                      Counting());
        QSignalSpy firstConnected(&first, &api::Api::Connected);
        QSignalSpy secondConnected(&second, &api::Api::Connected);
        first.Connect(Socket());
        second.Connect(Socket());
        QTRY_VERIFY_WITH_TIMEOUT(!firstConnected.isEmpty(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!secondConnected.isEmpty(), TIMEOUT_MS);
        QCOMPARE(Starts_.load(), 2);
        const qint64 pid = testing::DetachedDaemons::PidOf(Lock());
        QVERIFY(testing::DetachedDaemons::IsAlive(pid));
        // The loser has left, the holder of the lock is the one that is left.
        QTRY_COMPARE_WITH_TIMEOUT(CountDaemons(Dir_->path()), 1, TIMEOUT_MS);
        QCOMPARE(testing::DetachedDaemons::PidOf(Lock()), pid);
        QVERIFY(testing::DetachedDaemons::IsAlive(pid));
    }

    // A roostd that dies before it holds the socket lock (here: the directory
    // of the socket is open to others) says why in roostd.start.log, and the
    // launcher names that file when the start times out.
    void ARoostdThatDiesEarlySaysWhyInTheStartLog() {
        QVERIFY(QFile::setPermissions(Dir_->filePath("run"),
                                      QFile::ReadOwner | QFile::WriteOwner |
                                          QFile::ExeOwner | QFile::ReadGroup |
                                          QFile::ExeGroup | QFile::ReadOther |
                                          QFile::ExeOther));
        api::Api api(FastRetry());
        DaemonLauncher launcher(api, Command(), 500ms, Counting());
        QSignalSpy failed(&launcher, &DaemonLauncher::FailureChanged);
        api.Connect(Socket());
        QTRY_VERIFY_WITH_TIMEOUT(!launcher.Failure().isEmpty(), TIMEOUT_MS);
        QVERIFY2(launcher.Failure().contains("roostd.start.log"),
                 qPrintable(launcher.Failure()));
        QFile log(Dir_->filePath("run/roostd.start.log"));
        QVERIFY(log.open(QIODevice::ReadOnly));
        const QByteArray text = log.readAll();
        QVERIFY2(text.contains("chmod 700"), text.constData());
        QVERIFY(!QFile::exists(Dir_->filePath("run/roostd.log")));
        QVERIFY(!QFile::exists(Lock()));
    }

    // The program that does not exist: a failed start with the reason, no
    // start in flight, and the next NoListener tries again.
    void AProgramThatCannotStartIsAFailure() {
        api::Api api(FastRetry());
        DaemonLauncher launcher(
            api, {QStringLiteral("/nonexistent/roostd"), {}, QString()},
            DaemonLauncher::DEFAULT_START_TIMEOUT,
            [this](const DaemonLauncher::Command& command, QString& error) {
                ++Starts_;
                return DaemonLauncher::StartDetached(command, error);
            });
        api.Connect(Socket());
        QTRY_VERIFY_WITH_TIMEOUT(Starts_.load() >= 2, TIMEOUT_MS);
        QVERIFY2(launcher.Failure().startsWith("Could not start roostd: "),
                 qPrintable(launcher.Failure()));
        QVERIFY(!launcher.Starting());
    }

    // A success clears the failure.
    void AConnectionClearsTheFailure() {
        api::Api api(FastRetry());
        int calls = 0;
        DaemonLauncher launcher(
            api, Command(), DaemonLauncher::DEFAULT_START_TIMEOUT,
            [&calls](const DaemonLauncher::Command& command, QString& error) {
                if (++calls == 1) {
                    error = "first one fails";
                    return false;
                }
                return DaemonLauncher::StartDetached(command, error);
            });
        QSignalSpy connected(&api, &api::Api::Connected);
        api.Connect(Socket());
        QVERIFY(connected.wait(TIMEOUT_MS));
        QVERIFY(calls >= 2);
        QVERIFY2(launcher.Failure().isEmpty(), qPrintable(launcher.Failure()));
    }

    void TheAppImageIsUsedOnlyInsideItsAppDir_data() {
        QTest::addColumn<QString>("appImage");
        QTest::addColumn<QString>("appDir");
        QTest::addColumn<QString>("executableDir");
        QTest::addColumn<bool>("viaAppImage");
        QTest::newRow("inside") << "/x/Roost.AppImage" << "/tmp/.mount_a"
                                << "/tmp/.mount_a/usr/bin" << true;
        QTest::newRow("same dir") << "/x/Roost.AppImage" << "/tmp/.mount_a"
                                  << "/tmp/.mount_a/" << true;
        QTest::newRow("no appdir")
            << "/x/Roost.AppImage" << QString() << "/opt/roost/bin" << false;
        QTest::newRow("no appimage") << QString() << "/tmp/.mount_a"
                                     << "/tmp/.mount_a/usr/bin" << false;
        QTest::newRow("outside") << "/x/Roost.AppImage" << "/tmp/.mount_a"
                                 << "/opt/roost/bin" << false;
        QTest::newRow("sibling prefix")
            << "/x/Roost.AppImage" << "/tmp/.mount_a" << "/tmp/.mount_ab/bin"
            << false;
    }

    void TheAppImageIsUsedOnlyInsideItsAppDir() {
        QFETCH(QString, appImage);
        QFETCH(QString, appDir);
        QFETCH(QString, executableDir);
        QFETCH(bool, viaAppImage);
        const auto command = DaemonLauncher::CommandFor(
            "/run/roostd.sock", appImage, appDir, executableDir);
        if (viaAppImage) {
            QCOMPARE(command.Program, appImage);
            QCOMPARE(command.Arguments.first(), QString("--roostd"));
        } else {
            QCOMPARE(command.Program, executableDir + "/roostd");
            QVERIFY(!command.Arguments.contains("--roostd"));
        }
        QVERIFY(command.Arguments.contains("/run/roostd.log"));
        QCOMPARE(command.StartLog, QString("/run/roostd.start.log"));
    }

    // --no-spawn is the absence of a launcher: nobody is started.
    void WithoutALauncherNothingIsStarted() {
        api::Api api(FastRetry());
        QSignalSpy noListener(&api, &api::Api::NoListener);
        api.Connect(Socket());
        QTRY_VERIFY_WITH_TIMEOUT(noListener.size() >= 3, TIMEOUT_MS);
        QTest::qWait(300);
        QVERIFY(!QFile::exists(Socket()));
        QVERIFY(!QFile::exists(Lock()));
        QCOMPARE(CountDaemons(Dir_->path()), 0);
    }
};

QTEST_MAIN(DaemonLauncherTest)
#include "daemon_launcher_test.moc"
