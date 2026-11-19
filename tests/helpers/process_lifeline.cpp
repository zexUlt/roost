#include "helpers/process_lifeline.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>

namespace roost::testing {

namespace {

constexpr int WATCH_POLL_MS = 100;
constexpr int GRACE_POLLS = 50;  // 5 s between SIGTERM and SIGKILL.

// Runs in the watcher process, forked from the child before it execs, so only
// async-signal-safe calls are allowed. Does not return. The watcher belongs to
// the child process (its parent), outlives the test process by a moment at
// most and ends when the child does.
[[noreturn]] void Watch(int lifeline, pid_t child) {
    // Drop everything inherited but the read end of the pipe: a copy of the
    // write end of another lifeline would keep that one's pipe open, and the
    // standard streams would keep ctest waiting for the output.
    const int limit = std::min(::getdtablesize(), 65536);
    for (int fd = 0; fd < limit; ++fd) {
        if (fd != lifeline) {
            ::close(fd);
        }
    }
    pollfd watched{lifeline, POLLIN, 0};
    for (;;) {
        // The child is gone: nothing left to guard.
        if (::getppid() != child) {
            ::_exit(0);
        }
        if (::poll(&watched, 1, WATCH_POLL_MS) > 0) {
            break;  // Nobody writes, so EOF.
        }
    }
    // EOF also comes from Release(), which closes the pipe after the child has
    // left and been reaped, and its pid may already be reused. While the child
    // is still the parent, even a zombie, the pid cannot be reused.
    if (::getppid() != child) {
        ::_exit(0);
    }
    ::kill(child, SIGTERM);
    for (int i = 0; i < GRACE_POLLS && ::getppid() == child; ++i) {
        ::poll(nullptr, 0, WATCH_POLL_MS);
    }
    if (::getppid() == child) {
        ::kill(child, SIGKILL);
    }
    ::_exit(0);
}

}  // namespace

void RelayStandardError(QProcess& process) {
    process.setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(&process, &QProcess::readyReadStandardError, &process,
                     [&process] {
                         const QByteArray text = process.readAllStandardError();
                         std::fwrite(text.constData(), 1,
                                     static_cast<size_t>(text.size()), stderr);
                         std::fflush(stderr);
                     });
}

ProcessLifeline::~ProcessLifeline() { Release(); }

void ProcessLifeline::Release() {
    for (int* fd : {&Read_, &Write_}) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
}

bool ProcessLifeline::Attach(QProcess& process) {
    Release();
    // There is no PDEATHSIG on macOS, so a watcher, a fork of the child made
    // before it execs, reads the pipe whose write end only this process holds,
    // and stops the child when it closes. Both ends are close-on-exec, so the
    // child and its children do not hold them.
    int lifeline[2] = {-1, -1};
    if (::pipe(lifeline) != 0) {
        return false;
    }
    for (const int fd : lifeline) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    Read_ = lifeline[0];
    Write_ = lifeline[1];
    process.setChildProcessModifier([process = &process,
                                     watcherEnd = lifeline[0],
                                     lifelineWrite = lifeline[1]] {
        const pid_t child = ::getpid();
        const pid_t watcher = ::fork();
        if (watcher == 0) {
            ::close(lifelineWrite);
            Watch(watcherEnd, child);
        }
        if (watcher < 0) {
            // Without the watcher an aborted test would leave the child
            // behind, so the start fails instead.
            process->failChildProcessModifier("fork of the watcher", errno);
        }
    });
    return true;
}

}  // namespace roost::testing
