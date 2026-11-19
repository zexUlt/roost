#include "helpers/detached_daemons.hpp"

#include <QByteArray>
#include <QDeadlineTimer>
#include <QFile>
#include <QTest>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <vector>

namespace roost::testing {

namespace {

constexpr int GRACE_MS = 5000;

// The pid on the first line of the file, with async-signal-safe calls only (the
// watcher runs in a fork of a multi-threaded process): 0 if there is none.
long PidFromFile(const char* path) {
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    char buffer[32];
    const ssize_t size = ::read(fd, buffer, sizeof(buffer));
    ::close(fd);
    long pid = 0;
    for (ssize_t i = 0; i < size && buffer[i] >= '0' && buffer[i] <= '9'; ++i) {
        pid = pid * 10 + (buffer[i] - '0');
    }
    return pid;
}

// Runs in the watcher, a fork of the test process: async-signal-safe calls
// only. Does not return.
[[noreturn]] void Watch(int lifeline, const std::vector<const char*>& files) {
    ::setsid();
    pollfd watched{lifeline, POLLIN, 0};
    ::poll(&watched, 1, -1);
    char byte = 0;
    if (::read(lifeline, &byte, 1) == 1) {
        ::_exit(0);  // dismissed: the test cleaned up itself
    }
    // EOF without a byte: the test process is gone.
    for (const char* file : files) {
        const long pid = PidFromFile(file);
        if (pid > 1) {
            ::kill(static_cast<pid_t>(pid), SIGTERM);
        }
    }
    ::poll(nullptr, 0, GRACE_MS);
    for (const char* file : files) {
        const long pid = PidFromFile(file);
        if (pid > 1) {
            ::kill(static_cast<pid_t>(pid), SIGKILL);
        }
    }
    ::_exit(0);
}

}  // namespace

DetachedDaemons::DetachedDaemons(const QStringList& pidFiles)
    : PidFiles_(pidFiles) {
    int lifeline[2] = {-1, -1};
    if (::pipe(lifeline) != 0) {
        qFatal("DetachedDaemons: no pipe");
    }
    // Before the fork: nothing may allocate in the child.
    std::vector<QByteArray> paths;
    std::vector<const char*> files;
    for (const QString& file : pidFiles) {
        paths.push_back(QFile::encodeName(file));
    }
    for (const QByteArray& path : paths) {
        files.push_back(path.constData());
    }
    const pid_t watcher = ::fork();
    if (watcher < 0) {
        qFatal("DetachedDaemons: no fork");
    }
    if (watcher == 0) {
        // Only the read end is the watcher's; the standard streams would keep
        // ctest waiting for the output.
        ::close(lifeline[1]);
        const int limit = std::min(::getdtablesize(), 65536);
        for (int fd = 0; fd < limit; ++fd) {
            if (fd != lifeline[0]) {
                ::close(fd);
            }
        }
        Watch(lifeline[0], files);
    }
    ::close(lifeline[0]);
    ::fcntl(lifeline[1], F_SETFD, FD_CLOEXEC);
    Lifeline_ = lifeline[1];
}

DetachedDaemons::~DetachedDaemons() {
    KillAll();
    if (Lifeline_ >= 0) {
        const char byte = 1;
        [[maybe_unused]] const ssize_t written = ::write(Lifeline_, &byte, 1);
        ::close(Lifeline_);
    }
}

qint64 DetachedDaemons::PidOf(const QString& pidFile) {
    QFile file(pidFile);
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }
    bool ok = false;
    const qint64 pid = file.readLine().trimmed().toLongLong(&ok);
    return ok && pid > 1 ? pid : 0;
}

bool DetachedDaemons::IsAlive(qint64 pid) {
    return pid > 1 &&
           (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM);
}

void DetachedDaemons::KillAll() {
    QList<qint64> pids;
    for (const QString& file : PidFiles_) {
        if (const qint64 pid = PidOf(file); IsAlive(pid)) {
            pids.append(pid);
            ::kill(static_cast<pid_t>(pid), SIGTERM);
        }
    }
    const QDeadlineTimer deadline(GRACE_MS);
    for (const qint64 pid : pids) {
        while (IsAlive(pid) && !deadline.hasExpired()) {
            QTest::qWait(20);
        }
        if (IsAlive(pid)) {
            ::kill(static_cast<pid_t>(pid), SIGKILL);
        }
    }
}

}  // namespace roost::testing
