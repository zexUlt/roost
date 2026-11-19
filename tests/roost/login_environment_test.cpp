// LoginEnvironment and the launcher that uses it: the environment of the
// roostd that the window starts from the desktop. The shell is always a fake
// `$SHELL` script written into a temporary directory (the user's shell and
// dotfiles are never run): it ignores -l and -i, prints noise around the
// markers and runs the command it was given with a crafted environment.

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <chrono>
#include <memory>

#include "api/api.hpp"
#include "helpers/detached_daemons.hpp"
#include "helpers/scoped_env.hpp"
#include "roost/daemon_launcher.hpp"
#include "roost/login_environment.hpp"

using namespace roost;
using namespace std::chrono_literals;
using Settings = LoginEnvironment::Settings;
using testing::DetachedDaemons;
using testing::ScopedEnv;

namespace {

constexpr int TIMEOUT_MS = 30000;
constexpr const char* WARNING = "cannot read the login environment";

constexpr const char* FAKE_CLAUDE = R"sh(#!/bin/sh
dir=$(dirname "$0")
case "$1" in
agents) cat "$dir/agents.json" ;;
*) exit 2 ;;
esac
)sh";

// The shell of the cases: records its call, prints noise (one piece on the
// line of the marker), then runs the command with the environment of the
// login "shell" and prints more noise. @ENV@ is the list of the variables.
constexpr const char* FAKE_SHELL = R"sh(#!/bin/sh
echo "args=$1 $2 $3" > "@DIR@/ran"
echo "ld=$LD_LIBRARY_PATH" >> "@DIR@/ran"
printf 'noise of an rc file without a newline'
echo "motd: welcome"
echo "stderr noise" >&2
env -i @ENV@ /bin/sh -c "$4"
echo "trailing noise"
)sh";

bool WriteFile(const QString& path, const QByteArray& content,
               QFile::Permissions permissions = {}) {
    QFile file(path);
    if (!QDir().mkpath(QFileInfo(path).absolutePath()) ||
        !file.open(QIODevice::WriteOnly) ||
        file.write(content) != content.size()) {
        return false;
    }
    return permissions == QFile::Permissions{} ||
           file.setPermissions(permissions);
}

constexpr QFile::Permissions EXECUTABLE =
    QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;

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

QString ReadFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                          : QString();
}

}  // namespace

class LoginEnvironmentTest : public QObject {
    Q_OBJECT

private:
    QString Path(const QString& relative) const {
        return Dir_->filePath(relative);
    }
    QString Socket() const { return Path("run/roostd.sock"); }
    QString Lock() const { return Socket() + ".lock"; }

    // The fake shell with `variables` (NAME=value words) for the command.
    QString WriteShell(const QStringList& variables,
                       const QString& name = "shell") const {
        QByteArray script = FAKE_SHELL;
        script.replace("@DIR@", Dir_->path().toUtf8());
        script.replace("@ENV@", variables.join(' ').toUtf8());
        const QString path = Path(name);
        return WriteFile(path, script, EXECUTABLE) ? path : QString();
    }

    QString WriteScript(const QString& body, const QString& name) const {
        const QString path = Path(name);
        return WriteFile(path, "#!/bin/sh\n" + body.toUtf8(), EXECUTABLE)
                   ? path
                   : QString();
    }

    static Settings ForShell(const QString& shell) {
        Settings settings;
        settings.Base.insert("PATH", "/usr/bin:/bin");
        settings.Base.insert("FROM_WINDOW", "1");
        settings.HasTerminal = false;
        settings.Shell = shell;
        settings.Timeout = 20s;
        return settings;
    }

    std::unique_ptr<QTemporaryDir> Dir_;
    std::unique_ptr<DetachedDaemons> Daemons_;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
        QVERIFY(QDir().mkpath(Path("run")));
        QVERIFY(QFile::setPermissions(Path("run"), EXECUTABLE));
        QVERIFY(QDir().mkpath(Path("home")));
        Daemons_ = std::make_unique<DetachedDaemons>(QStringList{Lock()});
    }

    void cleanup() {
        Daemons_.reset();
        QTRY_COMPARE_WITH_TIMEOUT(CountDaemons(Dir_->path()), 0, TIMEOUT_MS);
        Dir_.reset();
    }

    // What the rc files print around the markers is ignored; the variables
    // are the ones of the shell, not of the window; a value may hold a
    // newline; the bundle's variables are stripped, and the library path of
    // the user stays outside an AppImage.
    void TheEnvironmentOfTheShellIsRead() {
        const QString shell = WriteShell(
            {"PATH=/login/bin:/usr/bin:/bin", "MULTI='a\nb'", "APPDIR=/mnt/app",
             "APPIMAGE=/x.AppImage", "ARGV0=roost", "OWD=/home/u",
             "LD_LIBRARY_PATH=/mine", "QT_PLUGIN_PATH=/mine/qt"});
        QVERIFY(!shell.isEmpty());
        LoginEnvironment login(ForShell(shell));
        QSignalSpy finished(&login, &LoginEnvironment::Finished);
        login.Start();
        QVERIFY(!login.Ready());
        QVERIFY(finished.wait(TIMEOUT_MS));
        QVERIFY(login.Ready());
        QVERIFY(login.FromLoginShell());
        const QProcessEnvironment environment = login.Environment();
        QCOMPARE(environment.value("PATH"), "/login/bin:/usr/bin:/bin");
        QCOMPARE(environment.value("MULTI"), "a\nb");
        QVERIFY(!environment.contains("FROM_WINDOW"));
        for (const QString& name : LoginEnvironment::BundleVariables()) {
            QVERIFY2(!environment.contains(name), qPrintable(name));
        }
        QCOMPARE(environment.value("LD_LIBRARY_PATH"), "/mine");
        QCOMPARE(environment.value("QT_PLUGIN_PATH"), "/mine/qt");
        QCOMPARE(ReadFile(Path("ran")).split('\n').first(), "args=-l -i -c");
    }

    // Inside an AppImage AppRun's variables go, the shell does not even see
    // them, and a second Start reads nothing again.
    void InsideAnAppImageTheLibraryVariablesAreStripped() {
        const QString shell =
            WriteShell({"PATH=/login/bin:/usr/bin:/bin",
                        "LD_LIBRARY_PATH=/app/lib", "QT_PLUGIN_PATH=/app/qt",
                        "QML2_IMPORT_PATH=/app/qml", "APPDIR=/app", "KEPT=1"});
        Settings settings = ForShell(shell);
        settings.InAppImage = true;
        settings.Base.insert("LD_LIBRARY_PATH", "/app/lib");
        LoginEnvironment login(settings);
        QSignalSpy finished(&login, &LoginEnvironment::Finished);
        login.Start();
        QVERIFY(finished.wait(TIMEOUT_MS));
        const QProcessEnvironment environment = login.Environment();
        QCOMPARE(environment.value("KEPT"), "1");
        QCOMPARE(environment.value("PATH"), "/login/bin:/usr/bin:/bin");
        QVERIFY(!environment.contains("LD_LIBRARY_PATH"));
        QVERIFY(!environment.contains("QT_PLUGIN_PATH"));
        QVERIFY(!environment.contains("QML2_IMPORT_PATH"));
        QVERIFY(!environment.contains("APPDIR"));
        QVERIFY2(ReadFile(Path("ran")).contains("ld=\n"),
                 qPrintable(ReadFile(Path("ran"))));
        login.Start();
        QCOMPARE(finished.size(), 1);
    }

    void StripDropsTheBundleVariables_data() {
        QTest::addColumn<bool>("inAppImage");
        QTest::addColumn<bool>("libraries");
        QTest::newRow("outside") << false << true;
        QTest::newRow("inside") << true << false;
    }

    void StripDropsTheBundleVariables() {
        QFETCH(bool, inAppImage);
        QFETCH(bool, libraries);
        QProcessEnvironment environment;
        for (const char* name :
             {"APPDIR", "APPIMAGE", "ARGV0", "OWD", "LD_LIBRARY_PATH",
              "QT_PLUGIN_PATH", "QML2_IMPORT_PATH", "PATH", "HOME"}) {
            environment.insert(name, "v");
        }
        const QProcessEnvironment stripped =
            LoginEnvironment::Strip(environment, inAppImage);
        for (const char* name : {"APPDIR", "APPIMAGE", "ARGV0", "OWD"}) {
            QVERIFY2(!stripped.contains(name), name);
        }
        for (const char* name :
             {"LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QML2_IMPORT_PATH"}) {
            QCOMPARE(stripped.contains(name), libraries);
        }
        QVERIFY(stripped.contains("PATH"));
        QVERIFY(stripped.contains("HOME"));
    }

    void ParseIgnoresWhatIsOutsideTheMarkers() {
        const QByteArray output = QByteArray("junk BEGIN") +
                                  QByteArray("A=1\0B=x=y\0\0no equals\0", 22) +
                                  "END more junk";
        const auto parsed = LoginEnvironment::Parse(output, "BEGIN", "END");
        QVERIFY(parsed);
        QCOMPARE(parsed->value("A"), "1");
        QCOMPARE(parsed->value("B"), "x=y");
        QCOMPARE(parsed->keys().size(), 2);
        QVERIFY(!LoginEnvironment::Parse("BEGIN A=1", "BEGIN", "END"));
        QVERIFY(!LoginEnvironment::Parse("A=1 END", "BEGIN", "END"));
    }

    // The shell that hangs: after the timeout the window's own environment is
    // used with a warning, and the shell and what it started are gone. The
    // event loop runs meanwhile.
    void AHangingShellFallsBackAfterTheTimeout() {
        const QString shell = WriteScript("echo $$ > \"" + Path("shell.pid") +
                                              "\"\nsleep 600 &\necho $! > \"" +
                                              Path("child.pid") + "\"\nwait\n",
                                          "hang");
        Settings settings = ForShell(shell);
        // Long enough for a loaded machine to run the script up to `wait`.
        settings.Timeout = 2000ms;
        settings.Base.insert("APPDIR", "/mnt/app");
        LoginEnvironment login(settings);
        QSignalSpy finished(&login, &LoginEnvironment::Finished);
        int ticks = 0;
        QTimer ticker;
        ticker.setInterval(10);
        connect(&ticker, &QTimer::timeout, this, [&ticks] { ++ticks; });
        ticker.start();
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(WARNING));
        QElapsedTimer started;
        started.start();
        login.Start();
        QVERIFY2(started.elapsed() < 1000, "Start waited for the shell");
        QVERIFY(!login.Ready());
        // The shell runs and has started its child before the timeout.
        QTRY_VERIFY_WITH_TIMEOUT(
            ReadFile(Path("child.pid")).trimmed().toLongLong() > 1, 1900);
        QVERIFY(finished.wait(TIMEOUT_MS));
        QVERIFY(started.elapsed() >= 1900);
        QVERIFY2(ticks >= 5, qPrintable(QString::number(ticks)));
        QVERIFY(login.Ready());
        QVERIFY(!login.FromLoginShell());
        QCOMPARE(login.Environment().value("FROM_WINDOW"), "1");
        QCOMPARE(login.Environment().value("PATH"), "/usr/bin:/bin");
        QVERIFY(!login.Environment().contains("APPDIR"));
        const qint64 shellPid =
            ReadFile(Path("shell.pid")).trimmed().toLongLong();
        const qint64 childPid =
            ReadFile(Path("child.pid")).trimmed().toLongLong();
        QVERIFY(shellPid > 1 && childPid > 1);
        QTRY_VERIFY_WITH_TIMEOUT(!DetachedDaemons::IsAlive(shellPid), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!DetachedDaemons::IsAlive(childPid), 5000);
    }

    // The shell that has printed everything and then hangs (an rc file that
    // does not exit): the read ends with the markers, not with the timeout,
    // and the shell and what it started are gone.
    void AShellThatHangsAfterPrintingIsNotWaitedFor() {
        const QString shell = WriteScript(
            "echo $$ > \"" + Path("shell.pid") +
                "\"\nsleep 600 &\necho $! > \"" + Path("child.pid") +
                "\"\nenv -i PATH=/login/bin:/usr/bin:/bin /bin/sh -c \"$4\"\n"
                "wait\n",
            "slowexit");
        Settings settings = ForShell(shell);
        settings.Timeout = 20s;
        LoginEnvironment login(settings);
        QSignalSpy finished(&login, &LoginEnvironment::Finished);
        QElapsedTimer started;
        started.start();
        login.Start();
        QVERIFY(finished.wait(TIMEOUT_MS));
        QVERIFY2(started.elapsed() < 10000,
                 qPrintable(QString::number(started.elapsed())));
        QVERIFY(login.FromLoginShell());
        QCOMPARE(login.Environment().value("PATH"), "/login/bin:/usr/bin:/bin");
        const qint64 shellPid =
            ReadFile(Path("shell.pid")).trimmed().toLongLong();
        const qint64 childPid =
            ReadFile(Path("child.pid")).trimmed().toLongLong();
        QVERIFY(shellPid > 1 && childPid > 1);
        QTRY_VERIFY_WITH_TIMEOUT(!DetachedDaemons::IsAlive(shellPid), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!DetachedDaemons::IsAlive(childPid), 5000);
    }

    void TheAppImageIsRecognisedByItsAppDir_data() {
        QTest::addColumn<QString>("appDir");
        QTest::addColumn<QString>("executableDir");
        QTest::addColumn<bool>("inside");
        QTest::newRow("inside")
            << "/tmp/.mount_a" << "/tmp/.mount_a/usr/bin" << true;
        QTest::newRow("same dir")
            << "/tmp/.mount_a" << "/tmp/.mount_a/" << true;
        QTest::newRow("no appdir") << QString() << "/opt/roost/bin" << false;
        QTest::newRow("outside")
            << "/tmp/.mount_a" << "/opt/roost/bin" << false;
        QTest::newRow("sibling prefix")
            << "/tmp/.mount_a" << "/tmp/.mount_ab/bin" << false;
    }

    void TheAppImageIsRecognisedByItsAppDir() {
        QFETCH(QString, appDir);
        QFETCH(QString, executableDir);
        QFETCH(bool, inside);
        QCOMPARE(LoginEnvironment::InAppImage(appDir, executableDir), inside);
    }

    // An extracted AppDir run through AppRun has APPDIR and no APPIMAGE: its
    // AppRun variables are still not for roostd.
    void AnExtractedAppDirWithoutAppImageCounts() {
        const ScopedEnv appImage("APPIMAGE", std::nullopt);
        {
            const ScopedEnv appDir(
                "APPDIR", QCoreApplication::applicationDirPath().toUtf8());
            QVERIFY(Settings::OfThisProcess().InAppImage);
        }
        {
            const ScopedEnv appDir("APPDIR", QByteArray("/nonexistent/appdir"));
            QVERIFY(!Settings::OfThisProcess().InAppImage);
        }
        const ScopedEnv appDir("APPDIR", std::nullopt);
        QVERIFY(!Settings::OfThisProcess().InAppImage);
    }

    void NoUsableShellIsAFallback_data() {
        QTest::addColumn<QString>("shell");
        QTest::addColumn<QString>("body");
        QTest::newRow("no SHELL") << QString() << QString();
        QTest::newRow("does not exist") << "/nonexistent/shell" << QString();
        QTest::newRow("no markers") << QString() << "echo only noise\n";
        QTest::newRow("no end marker")
            << QString() << "echo \"$4\" | cut -d\"'\" -f2\n";
        QTest::newRow("fails") << QString() << "exit 3\n";
    }

    void NoUsableShellIsAFallback() {
        QFETCH(QString, shell);
        QFETCH(QString, body);
        if (!body.isEmpty()) {
            shell = WriteScript(body, "odd");
            QVERIFY(!shell.isEmpty());
        }
        LoginEnvironment login(ForShell(shell));
        QSignalSpy finished(&login, &LoginEnvironment::Finished);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(WARNING));
        login.Start();
        if (!login.Ready()) {
            QVERIFY(finished.wait(TIMEOUT_MS));
        }
        QCOMPARE(finished.size(), 1);
        QVERIFY(!login.FromLoginShell());
        QCOMPARE(login.Environment().value("FROM_WINDOW"), "1");
    }

    // With a terminal the shell is not run: the window's environment.
    void WithATerminalTheShellIsNotRun() {
        const QString shell = WriteShell({"PATH=/login/bin"});
        Settings settings = ForShell(shell);
        settings.HasTerminal = true;
        settings.Base.insert("LD_LIBRARY_PATH", "/mine");
        settings.Base.insert("APPDIR", "/mnt/app");
        LoginEnvironment login(settings);
        QSignalSpy finished(&login, &LoginEnvironment::Finished);
        login.Start();
        QVERIFY(login.Ready());
        QCOMPARE(finished.size(), 1);
        QTest::qWait(200);
        QVERIFY(!QFile::exists(Path("ran")));
        QVERIFY(!login.FromLoginShell());
        QCOMPARE(login.Environment().value("PATH"), "/usr/bin:/bin");
        QCOMPARE(login.Environment().value("LD_LIBRARY_PATH"), "/mine");
        QVERIFY(!login.Environment().contains("APPDIR"));
    }

    // While the environment is read a NoListener starts nothing; the start
    // follows the read, once, with the environment of the shell.
    void TheLauncherStartsAfterTheRead() {
        const QString shell = WriteScript(
            "sleep 0.4\nenv -i PATH=/login/bin:/usr/bin:/bin /bin/sh -c "
            "\"$4\"\n",
            "slow");
        api::Api api({.FirstReconnectDelay = 20ms, .MaxReconnectDelay = 40ms});
        int starts = 0;
        std::optional<QProcessEnvironment> given;
        DaemonLauncher launcher(
            api, {"/unused", {}, QString()}, 1s,
            [&](const DaemonLauncher::Command& command, QString&) {
                ++starts;
                given = command.Environment;
                return true;
            },
            ForShell(shell));
        QSignalSpy noListener(&api, &api::Api::NoListener);
        api.Connect(Socket());
        QTRY_VERIFY_WITH_TIMEOUT(noListener.size() >= 3, TIMEOUT_MS);
        QVERIFY(launcher.Starting());
        QCOMPARE(starts, 0);
        QTRY_COMPARE_WITH_TIMEOUT(starts, 1, TIMEOUT_MS);
        QVERIFY(given);
        QCOMPARE(given->value("PATH"), "/login/bin:/usr/bin:/bin");
        QVERIFY(launcher.Starting());
    }

    // The real roostd, started by the launcher from a window whose own
    // environment is the bare PATH of the desktop. Returns the launcher's api
    // connected to it; `shell` is the login shell of the window.
    bool StartRoostdFromTheDesktop(const QString& shell, api::Api& api,
                                   std::unique_ptr<DaemonLauncher>& launcher) {
        Settings settings;
        settings.Base.insert("PATH", "/usr/bin:/bin");
        settings.Base.insert("HOME", Path("home"));
        settings.Base.insert("ROOST_SOCKET", Socket());
        settings.Base.insert("ROOST_CONFIG", Path("config"));
        settings.Base.insert("ROOST_PLUGINS_PATH", ROOST_PLUGINS_DIR);
        settings.HasTerminal = false;
        settings.Shell = shell;
        launcher = std::make_unique<DaemonLauncher>(
            api,
            DaemonLauncher::Command{QStringLiteral(ROOSTD_PATH),
                                    {"--log", Path("run/roostd.log")},
                                    Path("run/roostd.start.log")},
            DaemonLauncher::DEFAULT_START_TIMEOUT,
            &DaemonLauncher::StartDetached, settings);
        QSignalSpy connected(&api, &api::Api::Connected);
        api.Connect(Socket());
        return connected.wait(TIMEOUT_MS);
    }

    bool WriteFakeClaude() {
        return WriteFile(Path("claudebin/claude"), FAKE_CLAUDE, EXECUTABLE) &&
               QFile::copy(QStringLiteral(FIXTURES_DIR "/claude_agents.json"),
                           Path("claudebin/agents.json"));
    }

    // It lists the sessions of the fake `claude` only through the PATH of the
    // login shell.
    void RoostdFindsClaudeThroughTheLoginPath() {
        QVERIFY(WriteFakeClaude());
        const QString shell =
            WriteShell({"PATH=" + Path("claudebin") + ":/usr/bin:/bin",
                        "HOME=" + Path("home"), "ROOST_SOCKET=" + Socket(),
                        "ROOST_CONFIG=" + Path("config"),
                        "ROOST_PLUGINS_PATH=" ROOST_PLUGINS_DIR});
        api::Api api({.FirstReconnectDelay = 20ms, .MaxReconnectDelay = 40ms});
        std::unique_ptr<DaemonLauncher> launcher;
        QVERIFY(StartRoostdFromTheDesktop(shell, api, launcher));
        QTRY_COMPARE_WITH_TIMEOUT(api.Sessions().Rows().size(), 4, TIMEOUT_MS);
        QVERIFY(api.Sessions().Unavailable().isEmpty());
        QVERIFY(DetachedDaemons::IsAlive(DetachedDaemons::PidOf(Lock())));
    }

    // The same with a login read that fails: the roostd has the bare PATH of
    // the window, finds no `claude` and lists nothing.
    void RoostdWithoutTheLoginPathDoesNotFindClaude() {
        QVERIFY(WriteFakeClaude());
        const QString shell = WriteScript("exit 3\n", "failing");
        api::Api api({.FirstReconnectDelay = 20ms, .MaxReconnectDelay = 40ms});
        std::unique_ptr<DaemonLauncher> launcher;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(WARNING));
        QVERIFY(StartRoostdFromTheDesktop(shell, api, launcher));
        // sessions.list reports the agent as unavailable, with no rows.
        QTRY_COMPARE_WITH_TIMEOUT(api.Sessions().Unavailable().size(), 1,
                                  TIMEOUT_MS);
        const QJsonObject unavailable =
            api.Sessions().Unavailable().first().toObject();
        QCOMPARE(unavailable.value("agent").toString(), "claude");
        QVERIFY2(unavailable.value("reason").toString().contains(
                     "cannot run claude agents"),
                 qPrintable(unavailable.value("reason").toString()));
        QCOMPARE(api.Sessions().Rows().size(), 0);
    }
};

QTEST_MAIN(LoginEnvironmentTest)
#include "login_environment_test.moc"
