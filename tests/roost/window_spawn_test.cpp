// `roost --smoke-sessions` as a process with no roostd running: the window
// starts one, the roostd outlives the window (so does the attach it was asked
// for), and the next window finds the same roostd instead of starting another.
// `--no-spawn` starts nothing. Everything is in a temporary directory (HOME,
// ROOST_SOCKET, ROOST_CONFIG) behind a fake `claude`; the roostd the window
// starts is detached, so DetachedDaemons stops it by its lock and, if the
// test dies, a watcher does.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "helpers/detached_daemons.hpp"

using namespace roost;
using testing::DetachedDaemons;

namespace {

constexpr int TIMEOUT_MS = 60000;
constexpr const char* SESSION = "claude/bg-0a1b2c3d";

// A fake `claude`: `agents` prints the agents.json next to it, `attach <id>`
// writes its pid to attach.pid and stays until it is killed.
constexpr const char* FAKE_CLAUDE = R"sh(#!/bin/sh
dir=$(dirname "$0")
case "$1" in
agents) cat "$dir/agents.json" ;;
attach) echo $$ > "$dir/attach.pid.new" && mv "$dir/attach.pid.new" "$dir/attach.pid"; exec sleep 600 ;;
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

class WindowSpawnTest : public QObject {
    Q_OBJECT

private:
    QString Path(const char* relative) const {
        return Dir_->filePath(relative);
    }
    QString Lock() const { return Path("run/roostd.sock.lock"); }

    // The window as a process, to its end; the exit code, or -1.
    int RunWindow(const QStringList& arguments, QString& errors) {
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        environment.insert("QML_DISABLE_DISK_CACHE", "1");
        environment.insert("HOME", Path("home"));
        environment.insert("PATH", Path("bin") + ':' + qgetenv("PATH"));
        environment.insert("ROOST_SOCKET", Path("run/roostd.sock"));
        environment.insert("ROOST_CONFIG", Path("config"));
        environment.remove("ROOST_PLUGINS_PATH");
        environment.remove("APPIMAGE");
        QProcess window;
        window.setProcessEnvironment(environment);
        window.start(ROOST_PATH, arguments);
        if (!window.waitForFinished(TIMEOUT_MS)) {
            window.kill();
            window.waitForFinished(TIMEOUT_MS);
            errors = "the window did not end";
            return -1;
        }
        errors = QString::fromUtf8(window.readAllStandardError());
        return window.exitStatus() == QProcess::NormalExit ? window.exitCode()
                                                           : -1;
    }

    std::unique_ptr<QTemporaryDir> Dir_;
    std::unique_ptr<DetachedDaemons> Daemons_;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
        // roostd takes over the directory of the socket only if it is private.
        QVERIFY(QDir().mkpath(Path("run")));
        QVERIFY(QFile::setPermissions(Path("run"), QFile::ReadOwner |
                                                       QFile::WriteOwner |
                                                       QFile::ExeOwner));
        QVERIFY(QDir().mkpath(Path("home")));
        QVERIFY(
            WriteFile(Path("bin/claude"), FAKE_CLAUDE,
                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(QFile::copy(QStringLiteral(FIXTURES_DIR "/claude_agents.json"),
                            Path("bin/agents.json")));
        // The store document of the sessions plugin with one session opened,
        // so that the window attaches to it (design/08-state.md).
        const QByteArray document =
            QByteArray("{\"version\":1,\"view\":{\"opened\":\"") + SESSION +
            "\"}}";
        QVERIFY(WriteFile(Path("config/sessions.json"), document));
        Daemons_ = std::make_unique<DetachedDaemons>(
            QStringList{Lock(), Path("bin/attach.pid")});
    }

    void cleanup() {
        Daemons_.reset();
        QCOMPARE(CountDaemons(Dir_->path()), 0);
        Dir_.reset();
    }

    void TheWindowStartsARoostdThatOutlivesIt() {
        QString errors;
        const int code = RunWindow({"--smoke-sessions"}, errors);
        QVERIFY2(code == 0, qPrintable(errors));

        const qint64 pid = DetachedDaemons::PidOf(Lock());
        QVERIFY2(DetachedDaemons::IsAlive(pid), qPrintable(errors));
        // The window is gone; the session it opened is still attached.
        QTRY_VERIFY_WITH_TIMEOUT(
            DetachedDaemons::IsAlive(
                DetachedDaemons::PidOf(Path("bin/attach.pid"))),
            TIMEOUT_MS);
        const qint64 attach = DetachedDaemons::PidOf(Path("bin/attach.pid"));

        // The next window finds that roostd.
        const int again = RunWindow({"--smoke-sessions"}, errors);
        QVERIFY2(again == 0, qPrintable(errors));
        QCOMPARE(DetachedDaemons::PidOf(Lock()), pid);
        QVERIFY(DetachedDaemons::IsAlive(pid));
        QCOMPARE(CountDaemons(Dir_->path()), 1);
        QVERIFY(DetachedDaemons::IsAlive(attach));
    }

    void NoSpawnStartsNothing() {
        QString errors;
        const int code = RunWindow({"--no-spawn", "--smoke"}, errors);
        QVERIFY2(code == 0, qPrintable(errors));
        QVERIFY(!QFile::exists(Lock()));
        QVERIFY(!QFile::exists(Path("run/roostd.sock")));
        QCOMPARE(CountDaemons(Dir_->path()), 0);
    }
};

QTEST_MAIN(WindowSpawnTest)
#include "window_spawn_test.moc"
