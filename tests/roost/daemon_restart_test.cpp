// The "Restart roostd" button of the banner (design.md §4): the window of a
// newer package meets an older roostd. The old one is a helper process
// (refusing_daemon) that holds `<socket>.lock` as roostd does and refuses every
// hello with VersionMismatch; the window is the real shell with a real launcher
// and Api, and the new roostd is the real binary, detached (stopped through its
// lock by DetachedDaemons). Nothing is killed without the click, and on the
// click only the process that refused the window, if the lock names it.

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlApplicationEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

#include "api/api.hpp"
#include "helpers/detached_daemons.hpp"
#include "helpers/refusing_server.hpp"
#include "helpers/scoped_env.hpp"
#include "qml_api/roost_singleton.hpp"
#include "roost/daemon_launcher.hpp"
#include "roost/login_environment.hpp"
#include "roost/plugin_loader.hpp"
#include "roost/shell.hpp"

using namespace roost;
using namespace std::chrono_literals;

Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {
constexpr int TIMEOUT_MS = 20000;
// How long "nothing happens" is watched for.
constexpr int QUIET_MS = 700;
}  // namespace

class DaemonRestartTest : public QObject {
    Q_OBJECT

private:
    QString Socket() const { return Dir_->filePath("run/roostd.sock"); }
    QString Lock() const { return Socket() + ".lock"; }
    QString Marker() const { return Dir_->filePath("sigterm.marker"); }

    // The old roostd: the helper, with the lock as roostd takes it.
    // The lock carries the name of the executable, so the helper runs as
    // `<name>` in the directory of the case: a hard link, else a copy.
    QString Helper(const QString& name) {
        const QString path = Dir_->filePath("bin/" + name);
        if (!QFile::exists(path) &&
            ::link(REFUSING_DAEMON_PATH, qPrintable(path)) != 0) {
            if (!QFile::copy(REFUSING_DAEMON_PATH, path)) {
                qFatal("cannot make the helper %s", qPrintable(path));
            }
        }
        return path;
    }

    void StartOld(
        const QStringList& extra = {},
        const QString& name = QString(protocol::DAEMON_APPLICATION_NAME)) {
        Old_ = std::make_unique<QProcess>();
        Old_->setProgram(Helper(name));
        Old_->setArguments(QStringList{Socket(), "--marker", Marker()} + extra);
        Old_->start();
        QVERIFY(Old_->waitForStarted(TIMEOUT_MS));
        QTRY_VERIFY_WITH_TIMEOUT(Old_->canReadLine(), TIMEOUT_MS);
        QCOMPARE(Old_->readLine().trimmed(), QByteArray("ready"));
        QVERIFY(Old_->processId() > 1);
    }

    bool OldAlive() const {
        return Old_->state() != QProcess::NotRunning &&
               testing::DetachedDaemons::IsAlive(Old_->processId());
    }

    void Boot(
        bool withLauncher = true,
        std::chrono::milliseconds stopTimeout =
            DaemonLauncher::DEFAULT_STOP_TIMEOUT,
        std::optional<LoginEnvironment::Settings> environment = std::nullopt) {
        Api_ = std::make_unique<api::Api>(api::ApiOptions{
            .FirstReconnectDelay = 20ms, .MaxReconnectDelay = 40ms});
        if (withLauncher) {
            const DaemonLauncher::Command command{
                QStringLiteral(ROOSTD_PATH),
                {"--log", Dir_->filePath("run/roostd.log")},
                Dir_->filePath("run/roostd.start.log")};
            Launcher_ = std::make_unique<DaemonLauncher>(
                *Api_, command, 15s,
                [this](const DaemonLauncher::Command& toStart, QString& error) {
                    ++Starts_;
                    Started_ = toStart.Environment;
                    return DaemonLauncher::StartDetached(toStart, error);
                },
                std::move(environment), stopTimeout);
        }
        Engine_ = std::make_unique<QQmlApplicationEngine>();
        Singleton_ =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        QVERIFY(Singleton_ != nullptr);
        Singleton_->setApi(*Api_);
        Engine_->loadFromModule("Roost.Shell", "Main");
        QVERIFY(!Engine_->rootObjects().isEmpty());
        Loader_ = std::make_unique<PluginLoader>(*Engine_, Api_->Plugins(),
                                                 *Singleton_);
        Shell_ =
            std::make_unique<Shell>(*Engine_->rootObjects().first(), *Loader_,
                                    *Api_, Socket(), Launcher_.get());
        // After the shell listens: Connect may say Refused or Connected
        // inside the call, and a shell made later would not see it.
        Api_->Connect(Socket());
    }

    QObject* Root() { return Engine_->rootObjects().first(); }
    QString Connection() { return Root()->property("connection").toString(); }
    QQuickItem* Item(const char* name) {
        return Root()->findChild<QQuickItem*>(name);
    }
    QString BannerText() {
        return Item("connectionText")->property("text").toString();
    }
    bool Visible(const char* name) { return Item(name)->isVisible(); }
    void Click(const char* name) {
        QVERIFY(QMetaObject::invokeMethod(Item(name), "clicked"));
    }
    // The window is up against the old roostd: refused, the banner shown.
    void WaitRefused() {
        QTRY_COMPARE_WITH_TIMEOUT(Connection(), QString("refused"), TIMEOUT_MS);
    }

    QTemporaryDir* Dir_ = nullptr;
    std::unique_ptr<QTemporaryDir> Owned_;
    std::unique_ptr<testing::DetachedDaemons> Daemons_;
    std::vector<std::unique_ptr<testing::ScopedEnv>> Env_;
    std::unique_ptr<QProcess> Old_;
    std::unique_ptr<api::Api> Api_;
    std::unique_ptr<DaemonLauncher> Launcher_;
    std::unique_ptr<QQmlApplicationEngine> Engine_;
    RoostSingleton* Singleton_ = nullptr;
    std::unique_ptr<PluginLoader> Loader_;
    std::unique_ptr<Shell> Shell_;
    int Starts_ = 0;
    std::optional<QProcessEnvironment> Started_;

private slots:
    void init() {
        Owned_ = std::make_unique<QTemporaryDir>();
        Dir_ = Owned_.get();
        QVERIFY(Dir_->isValid());
        for (const char* name : {"run", "plugins", "home", "bin"}) {
            QVERIFY(QDir(Dir_->path()).mkpath(name));
        }
        QFile::setPermissions(Dir_->filePath("run"), QFile::ReadOwner |
                                                         QFile::WriteOwner |
                                                         QFile::ExeOwner);
        // Never the real ~/.config/roost or ~/.claude: the roostd the window
        // starts reads these.
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
        Started_.reset();
        Daemons_ =
            std::make_unique<testing::DetachedDaemons>(QStringList{Lock()});
    }

    void cleanup() {
        // The shell before the loader, the loader before the engine.
        Shell_.reset();
        Loader_.reset();
        Engine_.reset();
        Singleton_ = nullptr;
        Launcher_.reset();
        Api_.reset();
        if (Old_ && Old_->state() != QProcess::NotRunning) {
            Old_->kill();
            Old_->waitForFinished(TIMEOUT_MS);
        }
        Old_.reset();
        Daemons_.reset();
        Env_.clear();
        Owned_.reset();
    }

    // Both buttons and the text; and nothing is killed or started without the
    // click.
    void TheBannerOffersTheRestartAndNothingMovesWithoutTheClick() {
        StartOld();
        Boot();
        WaitRefused();
        QVERIFY(Visible("connectionBanner"));
        QVERIFY(Visible("retryButton"));
        QVERIFY(Visible("restartButton"));
        QVERIFY2(BannerText().contains("warm terminals"),
                 qPrintable(BannerText()));
        QVERIFY2(BannerText().contains("Claude sessions keep running"),
                 qPrintable(BannerText()));
        QTest::qWait(QUIET_MS);
        QVERIFY(OldAlive());
        QVERIFY(!QFile::exists(Marker()));
        QCOMPARE(Starts_, 0);
        QCOMPARE(Connection(), QString("refused"));
    }

    // Another refusal is not a version mismatch: no button, the old text.
    void NoButtonForAnotherRefusal() {
        testing::RefusingServer server;
        server.SetCode(protocol::EErrorCode::StoreFailed);
        QVERIFY(server.Listen(Socket()));
        Boot();
        WaitRefused();
        QVERIFY(Visible("retryButton"));
        QVERIFY(!Visible("restartButton"));
        QVERIFY2(!BannerText().contains("warm terminals"),
                 qPrintable(BannerText()));
    }

    // --no-spawn: no launcher, so nothing could start the new roostd.
    void NoButtonWithoutALauncher() {
        StartOld();
        Boot(false);
        WaitRefused();
        QVERIFY(Visible("retryButton"));
        QVERIFY(!Visible("restartButton"));
        QVERIFY2(!BannerText().contains("warm terminals"),
                 qPrintable(BannerText()));
        QTest::qWait(QUIET_MS);
        QVERIFY(OldAlive());
        QVERIFY(!QFile::exists(Marker()));
    }

    // The click: SIGTERM to the old one, the lock is released, the real roostd
    // is started and the window connects; the banner goes.
    void TheClickReplacesTheOldRoostd() {
        // Slow to go: the socket and the lock stay a while after SIGTERM, and
        // the new roostd must not be started before they are free.
        StartOld({"--exit-delay", "600"});
        const qint64 oldPid = Old_->processId();
        QCOMPARE(testing::DetachedDaemons::PidOf(Lock()), oldPid);
        Boot();
        WaitRefused();
        Click("restartButton");
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(Marker()), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Old_->state(), QProcess::NotRunning,
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Connection(), QString("connected"),
                                  TIMEOUT_MS);
        QVERIFY(!Visible("connectionBanner"));
        QVERIFY(!Visible("restartButton"));
        QVERIFY(Api_->IsConnected());
        // The lock is the new roostd's.
        const qint64 newPid = testing::DetachedDaemons::PidOf(Lock());
        QVERIFY(newPid > 1);
        QVERIFY(newPid != oldPid);
        QVERIFY(testing::DetachedDaemons::IsAlive(newPid));
        QCOMPARE(Starts_, 1);
    }

    // The old roostd answers hello with success but is of another build (an
    // update replaced the app under it): the same banner and the same restart
    // as for a refusal, nothing moves without the click.
    void ADaemonOfAnotherBuildGetsTheBannerAndTheRestart() {
        StartOld({"--exit-delay", "300", "--answer-version",
                  "0.0.1 (libghostty-vt 0.0.0)"});
        const qint64 oldPid = Old_->processId();
        Boot();
        WaitRefused();
        QVERIFY(Visible("restartButton"));
        QVERIFY2(BannerText().contains("0.0.1 (libghostty-vt 0.0.0)"),
                 qPrintable(BannerText()));
        QVERIFY2(BannerText().contains("warm terminals"),
                 qPrintable(BannerText()));
        QCOMPARE(Api_->RefusedPeerPid(), oldPid);
        QTest::qWait(QUIET_MS);
        QVERIFY(OldAlive());
        QVERIFY(!QFile::exists(Marker()));
        QCOMPARE(Starts_, 0);

        Click("restartButton");
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(Marker()), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Old_->state(), QProcess::NotRunning,
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Connection(), QString("connected"),
                                  TIMEOUT_MS);
        QVERIFY(!Visible("restartButton"));
        const qint64 newPid = testing::DetachedDaemons::PidOf(Lock());
        QVERIFY(newPid > 1);
        QVERIFY(newPid != oldPid);
        QCOMPARE(Starts_, 1);
    }

    // A roostd that does not tell its version counts as another build.
    void ADaemonWithoutAVersionGetsTheRestartToo() {
        StartOld({"--answer-no-version"});
        Boot();
        WaitRefused();
        QVERIFY(Visible("restartButton"));
        QVERIFY2(BannerText().contains("did not tell its version"),
                 qPrintable(BannerText()));
        QTest::qWait(QUIET_MS);
        QVERIFY(OldAlive());
        QCOMPARE(Starts_, 0);
    }

    // --no-spawn: the text does not repeat "restart roostd".
    void ADaemonOfAnotherBuildWithoutALauncherHasNoButton() {
        StartOld({"--answer-version", "0.0.1 (libghostty-vt 0.0.0)"});
        Boot(false);
        WaitRefused();
        QVERIFY(Visible("retryButton"));
        QVERIFY(!Visible("restartButton"));
        QVERIFY2(BannerText().contains("0.0.1 (libghostty-vt 0.0.0)"),
                 qPrintable(BannerText()));
        QVERIFY2(!BannerText().contains("Restart roostd"),
                 qPrintable(BannerText()));
        QVERIFY(OldAlive());
    }

    // A window from the desktop has not read the login environment when the
    // roostd it meets refuses it: the click reads it, then starts roostd with
    // it.
    void TheClickWaitsForTheLoginEnvironment() {
        // Not the user's shell: runs the command with one variable more, and
        // leaves a file to say it ran.
        const QString ran = Dir_->filePath("shell-ran");
        const QString shell = Dir_->filePath("bin/login-shell");
        QFile file(shell);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QStringLiteral("#!/bin/sh\n"
                                  ": > '%1'\n"
                                  "while [ \"$1\" != -c ]; do shift; done\n"
                                  "LOGIN_MARK=1 exec /bin/sh -c \"$2\"\n")
                       .arg(ran)
                       .toUtf8());
        file.close();
        QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                                    QFile::ExeOwner));
        LoginEnvironment::Settings settings;
        settings.Base = QProcessEnvironment::systemEnvironment();
        settings.HasTerminal = false;
        settings.Shell = shell;
        StartOld();
        Boot(true, DaemonLauncher::DEFAULT_STOP_TIMEOUT, settings);
        WaitRefused();
        QVERIFY(!QFile::exists(ran));
        Click("restartButton");
        QTRY_COMPARE_WITH_TIMEOUT(Connection(), QString("connected"),
                                  TIMEOUT_MS);
        QVERIFY(QFile::exists(ran));
        QCOMPARE(Starts_, 1);
        QVERIFY(Started_.has_value());
        QCOMPARE(Started_->value("LOGIN_MARK"), QString("1"));
    }

    // A lock of another program: nothing is killed, the banner says why.
    void ALockOfAnotherApplicationIsNotSignalled() {
        StartOld({}, "not-roostd");
        Boot();
        WaitRefused();
        Click("restartButton");
        QTRY_VERIFY_WITH_TIMEOUT(BannerText().contains("Could not restart"),
                                 TIMEOUT_MS);
        QVERIFY2(BannerText().contains("not-roostd"), qPrintable(BannerText()));
        QTest::qWait(QUIET_MS);
        QVERIFY(OldAlive());
        QVERIFY(!QFile::exists(Marker()));
        QCOMPARE(Starts_, 0);
        // The way back is open: the buttons stay.
        QCOMPARE(Connection(), QString("refused"));
        QVERIFY(Visible("restartButton"));
        QVERIFY(Visible("retryButton"));
    }

    // The lock names a different live roostd (another socket, or a reused
    // pid): the one that refused the window is not the one in the lock, so
    // nothing is signalled, neither of the two.
    void ALockThatNamesAnotherLiveProcessIsNotSignalled() {
        StartOld();
        // The other roostd: its own socket, lock and marker.
        const QString otherSocket = Dir_->filePath("run/other.sock");
        QProcess other;
        other.setProgram(Helper(QString(protocol::DAEMON_APPLICATION_NAME)));
        other.setArguments(
            {otherSocket, "--marker", Dir_->filePath("other.marker")});
        other.start();
        QVERIFY(other.waitForStarted(TIMEOUT_MS));
        QTRY_VERIFY_WITH_TIMEOUT(other.canReadLine(), TIMEOUT_MS);
        QCOMPARE(other.readLine().trimmed(), QByteArray("ready"));
        const qint64 otherPid = other.processId();
        QVERIFY(otherPid > 1);
        QVERIFY(otherPid != Old_->processId());
        // The lock of the one that serves the socket now names the other.
        QFile file(Lock());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QList<QByteArray> lines = file.readAll().split('\n');
        file.close();
        QVERIFY(lines.size() >= 3);
        QCOMPARE(lines[0].toLongLong(), Old_->processId());
        lines[0] = QByteArray::number(otherPid);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(lines.join('\n'));
        file.close();
        Boot();
        WaitRefused();
        Click("restartButton");
        QTRY_VERIFY_WITH_TIMEOUT(BannerText().contains("Could not restart"),
                                 TIMEOUT_MS);
        QVERIFY2(BannerText().contains(QString::number(otherPid)),
                 qPrintable(BannerText()));
        QTest::qWait(QUIET_MS);
        const bool otherAlive = other.state() != QProcess::NotRunning &&
                                testing::DetachedDaemons::IsAlive(otherPid);
        const bool otherSignalled =
            QFile::exists(Dir_->filePath("other.marker"));
        other.kill();
        other.waitForFinished(TIMEOUT_MS);
        QVERIFY(otherAlive);
        QVERIFY(!otherSignalled);
        QVERIFY(OldAlive());
        QVERIFY(!QFile::exists(Marker()));
        QCOMPARE(Starts_, 0);
        QCOMPARE(Connection(), QString("refused"));
        QVERIFY(Visible("restartButton"));
    }

    // An old roostd that does not leave: after the (short, injected) wait the
    // banner says so; it got SIGTERM and nothing else, and no roostd is started
    // over it.
    void AnOldRoostdThatStaysIsNotKilledTwice() {
        StartOld({"--ignore-term"});
        Boot(true, 400ms);
        WaitRefused();
        Click("restartButton");
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(Marker()), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(BannerText().contains("did not exit"),
                                 TIMEOUT_MS);
        QCOMPARE(Connection(), QString("refused"));
        QVERIFY(Visible("restartButton"));
        QTest::qWait(QUIET_MS);
        QVERIFY(OldAlive());
        QCOMPARE(Starts_, 0);
    }
};

QTEST_MAIN(DaemonRestartTest)
#include "daemon_restart_test.moc"
