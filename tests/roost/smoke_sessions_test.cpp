// `roost --smoke-sessions` as a process, the way a package check runs it: the
// real roostd with the built-in plugins behind a fake `claude`, in a temporary
// ROOST_CONFIG/ROOST_SOCKET/ROOST_PLUGINS_PATH. It exits 0 when the sessions
// are listed in the window; it exits 1 with the reason when the sessions
// plugin has lost its QML module (RoostSessions), and when claude lists no
// sessions (after the timeout of the mode, so that case takes as long). When
// the store marks a session as opened, the window attaches to it at start and
// the mode exits 0 only once the attach has started; it exits 1 with the
// reason when roostd refuses to open that session.

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

#include <csignal>
#include <memory>

#include "helpers/roostd_process.hpp"
#include "helpers/scoped_env.hpp"

using namespace roost;

namespace {

constexpr int TIMEOUT_MS = 10000;
// The mode gives up after 20 s (src/roost/main.cpp); and the window starts.
constexpr int NOT_READY_WAIT_MS = 60000;

// The session the store marks as opened (a session of claude_agents.json), and
// the one of the fixture agent, which cannot be attached to.
constexpr const char* OPENED = "claude/bg-0a1b2c3d";
constexpr const char* UNATTACHABLE = "fx-agent/s1";

// A fake `claude`: `agents` prints the agents.json next to it, `attach <id>`
// writes its pid to attach.pid (the file is there whole or not at all) and
// stays until it is killed.
constexpr const char* FAKE_CLAUDE = R"sh(#!/bin/sh
dir=$(dirname "$0")
case "$1" in
agents) cat "$dir/agents.json" ;;
attach) echo $$ > "$dir/attach.pid.new" && mv "$dir/attach.pid.new" "$dir/attach.pid"; exec sleep 60 ;;
*) exit 2 ;;
esac
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

QByteArray ReadFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

}  // namespace

class SmokeSessionsTest : public QObject {
    Q_OBJECT

private:
    // The fake claude with the given `claude agents --json` output.
    bool WriteClaude(const QByteArray& agents) const {
        const QString bin = Bin_.path();
        return WriteFile(
                   bin + "/claude", FAKE_CLAUDE,
                   QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner) &&
               WriteFile(bin + "/agents.json", agents);
    }

    // The built-in plugins in the plugin directory of the daemon, by link.
    // The plugin `broken` is a copy of plugin.json and the panel of the plugin
    // without something it needs: `sessions` without its QML module, `terminal`
    // with a QML file that does not parse. `badManifest` adds a directory
    // whose plugin.json is not JSON, which roostd reports as a problem.
    enum class EFault {
        None,
        SessionsWithoutModule,
        BrokenTerminal,
        BadManifest
    };

    bool LinkPlugins(EFault fault) const {
        const QString plugins = Daemon_->PluginsPath();
        if (!QDir().mkpath(plugins)) {
            return false;
        }
        const QString source = QStringLiteral(ROOST_PLUGINS_DIR);
        const QString copied =
            fault == EFault::SessionsWithoutModule ? QStringLiteral("sessions")
            : fault == EFault::BrokenTerminal      ? QStringLiteral("terminal")
                                                   : QString();
        for (const QString& id :
             {QStringLiteral("claude"), QStringLiteral("terminal"),
              QStringLiteral("sessions")}) {
            if (id != copied &&
                !QFile::link(source + "/" + id, plugins + "/" + id)) {
                return false;
            }
        }
        if (fault == EFault::BadManifest) {
            return WriteFile(plugins + "/garbled/plugin.json", "{ not json");
        }
        if (copied.isEmpty()) {
            return true;
        }
        const QString from = source + "/" + copied;
        const QString panel = copied == QLatin1String("sessions")
                                  ? QStringLiteral("SessionsPanel.qml")
                                  : QStringLiteral("TerminalPanel.qml");
        return WriteFile(plugins + "/" + copied + "/plugin.json",
                         ReadFile(from + "/plugin.json")) &&
               WriteFile(plugins + "/" + copied + "/qml/" + panel,
                         fault == EFault::BrokenTerminal
                             ? QByteArray("import QtQuick\nItem { ][ }\n")
                             : ReadFile(from + "/qml/" + panel));
    }

    // roostd with the plugins and the fake claude; false if any step failed.
    // `opened` is the session the store marks as opened, so that the window
    // attaches to it at start; with `fixtureAgent` roostd also lists the
    // session of an agent that cannot be attached to.
    bool Prepare(EFault fault, const QByteArray& agents,
                 const QString& opened = {}, bool fixtureAgent = false) {
        Daemon_ = std::make_unique<testing::RoostdProcess>();
        if (!Bin_.isValid() || !Daemon_->IsValid() || !WriteClaude(agents) ||
            !LinkPlugins(fault)) {
            return false;
        }
        if (fixtureAgent && !Daemon_->AddFixturePlugin(
                                "fx", QStringLiteral(FIXTURE_BACKEND_PATH))) {
            return false;
        }
        // The store document of the sessions plugin (design/08-state.md).
        if (!opened.isEmpty() &&
            !WriteFile(Daemon_->ConfigPath() + "/sessions.json",
                       QByteArray("{\"version\":1,\"view\":{\"opened\":\"") +
                           opened.toUtf8() + "\"}}")) {
            return false;
        }
        return StartDaemon();
    }

    QString AttachPidFile() const { return Bin_.filePath("attach.pid"); }

    bool StartDaemon() {
        // The daemon inherits the environment of this process.
        Path_ = std::make_unique<testing::ScopedEnv>(
            "PATH", Bin_.path().toUtf8() + ':' + qgetenv("PATH"));
        return Daemon_->Start(QStringLiteral(ROOSTD_PATH));
    }

    // Runs `roost --smoke-sessions` against the daemon to its end.
    void RunWindow(int waitMs, QElapsedTimer& elapsed) {
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        environment.insert("QML_DISABLE_DISK_CACHE", "1");
        environment.insert("ROOST_SOCKET", Daemon_->SocketPath());
        environment.insert("ROOST_CONFIG", Daemon_->ConfigPath());
        Window_.setProcessEnvironment(environment);
        elapsed.start();
        // --no-spawn: the daemon of the case is up, and a window that lost it
        // must fail the case, not start a detached roostd that outlives it.
        Window_.start(ROOST_PATH, {"--no-spawn", "--smoke-sessions"});
        if (!Window_.waitForFinished(waitMs)) {
            Window_.kill();
            Window_.waitForFinished(TIMEOUT_MS);
            QFAIL("roost --smoke-sessions did not end");
        }
        QCOMPARE(Window_.exitStatus(), QProcess::NormalExit);
        Errors_ = QString::fromUtf8(Window_.readAllStandardError());
    }

    QTemporaryDir Bin_;
    std::unique_ptr<testing::RoostdProcess> Daemon_;
    std::unique_ptr<testing::ScopedEnv> Path_;
    QProcess Window_;
    QString Errors_;

private slots:
    void cleanup() {
        if (Window_.state() != QProcess::NotRunning) {
            Window_.kill();
            Window_.waitForFinished(TIMEOUT_MS);
        }
        // The daemon first: it uses the PATH that Path_ restores.
        Daemon_.reset();
        Path_.reset();
        // The fake attach, if roostd left it behind.
        if (const qint64 pid = ReadFile(AttachPidFile()).trimmed().toLongLong();
            pid > 1) {
            ::kill(static_cast<pid_t>(pid), SIGKILL);
        }
        // Bin_ lives as long as the test object: the next case starts clean.
        QFile::remove(AttachPidFile());
    }

    void ExitsZeroWhenTheSessionsAreListed() {
        QVERIFY(Prepare(
            EFault::None,
            ReadFile(QStringLiteral(FIXTURES_DIR "/claude_agents.json"))));
        QElapsedTimer elapsed;
        RunWindow(TIMEOUT_MS * 3, elapsed);
        QVERIFY2(Window_.exitCode() == 0, qPrintable(Errors_));
        qInfo("ready in %lld ms", static_cast<long long>(elapsed.elapsed()));
    }

    // The window opens the session the store marks as opened, and the mode
    // does not end before roostd has started the attach to it: the attach is
    // there as soon as the window has exited. The file is not waited for: a
    // window that exits with the open sent but not answered still gets its
    // attach started a moment later, and waiting would hide that.
    void WaitsForTheAttachOfTheOpenedSession() {
        QVERIFY(Prepare(
            EFault::None,
            ReadFile(QStringLiteral(FIXTURES_DIR "/claude_agents.json")),
            OPENED));
        QElapsedTimer elapsed;
        RunWindow(TIMEOUT_MS * 3, elapsed);
        QVERIFY2(Window_.exitCode() == 0, qPrintable(Errors_));
        QVERIFY2(QFile::exists(AttachPidFile()), "the attach had not started");
        qInfo("ready in %lld ms", static_cast<long long>(elapsed.elapsed()));
    }

    // No session is opened at start: nothing is waited for and nothing is
    // attached.
    void WaitsForNoAttachWhenNoSessionIsOpened() {
        QVERIFY(Prepare(
            EFault::None,
            ReadFile(QStringLiteral(FIXTURES_DIR "/claude_agents.json"))));
        QElapsedTimer elapsed;
        RunWindow(TIMEOUT_MS * 3, elapsed);
        QVERIFY2(Window_.exitCode() == 0, qPrintable(Errors_));
        QVERIFY(!QFile::exists(AttachPidFile()));
    }

    // roostd refuses to open the session of the store: the mode fails at once
    // with the reason, not after its timeout.
    void FailsWhenTheOpenedSessionCannotBeOpened() {
        QVERIFY(Prepare(
            EFault::None,
            ReadFile(QStringLiteral(FIXTURES_DIR "/claude_agents.json")),
            UNATTACHABLE, true));
        QElapsedTimer elapsed;
        RunWindow(TIMEOUT_MS * 3, elapsed);
        QCOMPARE(Window_.exitCode(), 1);
        QVERIFY2(Errors_.contains("roost --smoke-sessions: panel terminal.view "
                                  "could not open fx-agent/s1: "),
                 qPrintable(Errors_));
        // Well under SMOKE_SESSIONS_TIMEOUT (20 s), with room for a loaded
        // machine.
        QVERIFY2(elapsed.elapsed() < 10000, qPrintable(Errors_));
        qInfo("failed in %lld ms", static_cast<long long>(elapsed.elapsed()));
    }

    // A package that lacks the QML module of the sessions plugin: --smoke
    // passes such a window, --smoke-sessions must not.
    void FailsWhenTheSessionsQmlModuleIsMissing() {
        QVERIFY(Prepare(
            EFault::SessionsWithoutModule,
            ReadFile(QStringLiteral(FIXTURES_DIR "/claude_agents.json"))));
        QElapsedTimer elapsed;
        RunWindow(TIMEOUT_MS * 3, elapsed);
        QCOMPARE(Window_.exitCode(), 1);
        QVERIFY2(Errors_.contains("roost --smoke-sessions: plugin sessions") &&
                     Errors_.contains("RoostSessions"),
                 qPrintable(Errors_));
        qInfo("failed in %lld ms", static_cast<long long>(elapsed.elapsed()));
    }

    // The terminal panel does not load: the reason names terminal.view.
    void FailsWhenTheTerminalPanelDoesNotLoad() {
        QVERIFY(Prepare(
            EFault::BrokenTerminal,
            ReadFile(QStringLiteral(FIXTURES_DIR "/claude_agents.json"))));
        QElapsedTimer elapsed;
        RunWindow(TIMEOUT_MS * 3, elapsed);
        QCOMPARE(Window_.exitCode(), 1);
        QVERIFY2(Errors_.contains("roost --smoke-sessions: plugin terminal") &&
                     Errors_.contains("terminal.view"),
                 qPrintable(Errors_));
        QVERIFY2(elapsed.elapsed() < 3000, qPrintable(Errors_));
        qInfo("failed in %lld ms", static_cast<long long>(elapsed.elapsed()));
    }

    // A plugin roostd could not read: its problem is the reason.
    void FailsWhenRoostdReportsAPluginProblem() {
        QVERIFY(Prepare(
            EFault::BadManifest,
            ReadFile(QStringLiteral(FIXTURES_DIR "/claude_agents.json"))));
        QElapsedTimer elapsed;
        RunWindow(TIMEOUT_MS * 3, elapsed);
        QCOMPARE(Window_.exitCode(), 1);
        QVERIFY2(Errors_.contains("roost --smoke-sessions: roostd: ") &&
                     Errors_.contains("garbled"),
                 qPrintable(Errors_));
        QVERIFY2(elapsed.elapsed() < 3000, qPrintable(Errors_));
        qInfo("failed in %lld ms", static_cast<long long>(elapsed.elapsed()));
    }

    // Both check modes at once is a mistake of the caller; no daemon needed.
    void RejectsSmokeTogetherWithSmokeSessions() {
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        Window_.setProcessEnvironment(environment);
        QElapsedTimer elapsed;
        elapsed.start();
        Window_.start(ROOST_PATH, {"--smoke", "--smoke-sessions"});
        QVERIFY(Window_.waitForFinished(TIMEOUT_MS));
        QCOMPARE(Window_.exitStatus(), QProcess::NormalExit);
        Errors_ = QString::fromUtf8(Window_.readAllStandardError());
        QCOMPARE(Window_.exitCode(), 1);
        QVERIFY2(Errors_.contains("--smoke does not go with --smoke-sessions"),
                 qPrintable(Errors_));
        QVERIFY2(elapsed.elapsed() < 3000, qPrintable(Errors_));
    }

    void FailsAfterTheTimeoutWhenThereAreNoSessions() {
        QVERIFY(Prepare(EFault::None, "[]"));
        QElapsedTimer elapsed;
        RunWindow(NOT_READY_WAIT_MS, elapsed);
        QCOMPARE(Window_.exitCode(), 1);
        QVERIFY2(Errors_.contains("not ready within 20 s") &&
                     Errors_.contains("roostd lists no sessions"),
                 qPrintable(Errors_));
        qInfo("failed in %lld ms", static_cast<long long>(elapsed.elapsed()));
    }
};

QTEST_MAIN(SmokeSessionsTest)
#include "smoke_sessions_test.moc"
