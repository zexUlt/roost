#include "helpers/roostd_process.hpp"

#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QTest>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

#include "core/rpc_client.hpp"

namespace roost::testing {

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

constexpr int WATCH_POLL_MS = 100;
constexpr int GRACE_POLLS = 50;  // 5 s between SIGTERM and SIGKILL.

// Runs in the watcher process, forked from the child before it execs roostd, so
// only async-signal-safe calls are allowed. Does not return. The watcher
// belongs to the roostd process (its parent), outlives the test process by a
// moment at most and ends when roostd does.
[[noreturn]] void Watch(int lifeline, pid_t roostd) {
    // Drop everything inherited but the read end of the pipe: a copy of the
    // write end of another RoostdProcess would keep that one's pipe open, and
    // the standard streams would keep ctest waiting for the output.
    const int limit = std::min(::getdtablesize(), 65536);
    for (int fd = 0; fd < limit; ++fd) {
        if (fd != lifeline) {
            ::close(fd);
        }
    }
    pollfd watched{lifeline, POLLIN, 0};
    for (;;) {
        // roostd gone: nothing left to guard.
        if (::getppid() != roostd) {
            ::_exit(0);
        }
        if (::poll(&watched, 1, WATCH_POLL_MS) > 0) {
            break;  // Nobody writes, so EOF.
        }
    }
    // EOF also comes from Stop(), which closes the pipe after roostd has left
    // and been reaped, and its pid may already be reused. While roostd is still
    // the parent, even a zombie, the pid cannot be reused.
    if (::getppid() != roostd) {
        ::_exit(0);
    }
    ::kill(roostd, SIGTERM);
    for (int i = 0; i < GRACE_POLLS && ::getppid() == roostd; ++i) {
        ::poll(nullptr, 0, WATCH_POLL_MS);
    }
    if (::getppid() == roostd) {
        ::kill(roostd, SIGKILL);
    }
    ::_exit(0);
}

}  // namespace

RoostdProcess::RoostdProcess() = default;

RoostdProcess::~RoostdProcess() { Stop(); }

void RoostdProcess::CloseLifelineOfWatcher() {
    if (WatcherLifeline_ >= 0) {
        ::close(WatcherLifeline_);
        WatcherLifeline_ = -1;
    }
}

bool RoostdProcess::AddFixturePlugin(const QString& id,
                                     const QString& fixtureBackendPath) {
    const QDir directory(PluginsPath() + u'/' + id);
    if (!directory.mkpath(".") ||
        !QFile::link(fixtureBackendPath, directory.filePath("backend"))) {
        return false;
    }
    QJsonArray commands;
    for (const char* command : {"fixture.echo", "fixture.pid", "fixture.crash",
                                "fixture.hang", "fixture.notify"}) {
        commands.append(QJsonObject{{"id", command}, {"title", command}});
    }
    const QJsonObject manifest{
        {"id", id},
        {"version", "1"},
        {"api", "0"},
        {"backend", QJsonObject{{"command", QJsonArray{"backend"}}}},
        {"contributes",
         QJsonObject{
             {"commands", commands},
             {"agents", QJsonArray{QJsonObject{
                            {"id", id + "-agent"},
                            {"capabilities", QJsonArray{"nativeTuiAttach"}}}}},
         }},
    };
    // What the backend answers to agent.listSessions; see fixture_backend.cpp.
    const QJsonObject answers{
        {"agent.listSessions",
         QJsonObject{{id + "-agent",
                      QJsonObject{{"result",
                                   QJsonArray{QJsonObject{
                                       {"id", "s1"}, {"state", "idle"}}}}}}}}};
    QFile manifestFile(directory.filePath("plugin.json"));
    QFile answersFile(directory.filePath("agent.json"));
    return manifestFile.open(QIODevice::WriteOnly) &&
           manifestFile.write(QJsonDocument(manifest).toJson()) > 0 &&
           answersFile.open(QIODevice::WriteOnly) &&
           answersFile.write(QJsonDocument(answers).toJson()) > 0;
}

bool RoostdProcess::AddQmlPlugin(const QString& id, const QJsonArray& panels,
                                 const QMap<QString, QByteArray>& files,
                                 const QString& version,
                                 const QJsonValue& native) {
    const QDir directory(PluginsPath() + u'/' + id);
    if (!directory.mkpath(".")) {
        return false;
    }
    for (auto it = files.begin(); it != files.end(); ++it) {
        QFile file(directory.filePath(it.key()));
        if (!QDir().mkpath(QFileInfo(file).absolutePath()) ||
            !file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(it.value()) != it.value().size()) {
            return false;
        }
    }
    QJsonObject manifest{
        {"id", id},
        {"version", version},
        {"api", "0"},
        {"contributes", QJsonObject{{"panels", panels}}},
    };
    if (!native.isUndefined()) {
        manifest.insert("native", native);
    }
    QFile manifestFile(directory.filePath("plugin.json"));
    return manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           manifestFile.write(QJsonDocument(manifest).toJson()) > 0;
}

bool RoostdProcess::Start(const QString& roostdPath) {
    if (!IsValid() || IsRunning()) {
        return false;
    }
    Process_ = std::make_unique<QProcess>();
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("ROOST_SOCKET", SocketPath());
    environment.insert("ROOST_CONFIG", ConfigPath());
    environment.insert("ROOST_PLUGINS_PATH", PluginsPath());
    Process_->setProcessEnvironment(environment);
    // There is no PDEATHSIG on macOS, so a watcher, a fork of the child made
    // before it execs roostd, reads the pipe whose write end only this process
    // holds, and stops roostd when it closes. Both ends are close-on-exec, so
    // roostd and its children do not hold them.
    CloseLifelineOfWatcher();
    int lifeline[2] = {-1, -1};
    if (::pipe(lifeline) != 0) {
        return false;
    }
    for (const int fd : lifeline) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    WatcherLifeline_ = lifeline[1];
    const int watcherEnd = lifeline[0];
    Process_->setChildProcessModifier(
        [process = Process_.get(), watcherEnd, lifelineWrite = lifeline[1]] {
            const pid_t roostd = ::getpid();
            const pid_t watcher = ::fork();
            if (watcher == 0) {
                ::close(lifelineWrite);
                Watch(watcherEnd, roostd);
            }
            if (watcher < 0) {
                // Without the watcher an aborted test would leave roostd
                // behind, so the start fails instead.
                process->failChildProcessModifier("fork of the roostd watcher",
                                                  errno);
            }
        });
    Process_->start(roostdPath, {});
    ::close(watcherEnd);
    QElapsedTimer elapsed;
    elapsed.start();
    if (!Process_->waitForStarted(TIMEOUT_MS)) {
        qWarning().noquote()
            << "roostd did not start after" << elapsed.elapsed()
            << "ms:" << Process_->errorString();
        return false;
    }
    if (!Greets(SocketPath())) {
        qWarning().noquote()
            << "roostd did not answer hello after" << elapsed.elapsed()
            << "ms; process state" << Process_->state() << "exit code"
            << Process_->exitCode() << "stderr:"
            << QString::fromLocal8Bit(Process_->readAllStandardError());
        return false;
    }
    return true;
}

bool RoostdProcess::Stop() {
    if (!Process_ || Process_->state() == QProcess::NotRunning) {
        CloseLifelineOfWatcher();
        return true;
    }
    Process_->terminate();
    const bool left = Process_->waitForFinished(TIMEOUT_MS);
    if (!left) {
        Process_->kill();
        Process_->waitForFinished(TIMEOUT_MS);
    }
    CloseLifelineOfWatcher();
    return left;
}

bool RoostdProcess::IsRunning() const {
    return Process_ && Process_->state() != QProcess::NotRunning;
}

}  // namespace roost::testing
