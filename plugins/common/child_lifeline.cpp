#include "common/child_lifeline.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>

namespace roost::plugin {

namespace {

constexpr int WATCH_POLL_MS = 100;

// Runs in the watchdog, forked from the child before it execs, so only
// async-signal-safe calls are allowed. Does not return. The watchdog is a
// child of the command and in its group, so that the kill on a timeout takes
// it too.
[[noreturn]] void Watch(int lifeline, pid_t command) {
    // Drop everything inherited but the read end: a copy of the write end of
    // the lifeline of another command would keep that pipe open, and the
    // standard streams and the pipe of the exec status would keep the parent
    // waiting for the end of the command.
    const int limit = std::min(::getdtablesize(), 65536);
    for (int fd = 0; fd < limit; ++fd) {
        if (fd != lifeline) {
            ::close(fd);
        }
    }
    pollfd watched{lifeline, POLLIN, 0};
    for (;;) {
        // The command is gone and the watchdog was handed to init: nothing
        // left to guard.
        if (::getppid() != command) {
            ::_exit(0);
        }
        if (::poll(&watched, 1, WATCH_POLL_MS) > 0) {
            break;  // Nobody writes, so EOF.
        }
    }
    // EOF also comes from a normal end of the run, when the command has left
    // and its pid may be reused. While the command is still the parent, even
    // as a zombie, the pid and its group cannot belong to anyone else.
    if (::getppid() == command) {
        ::killpg(command, SIGKILL);
    }
    ::_exit(0);
}

}  // namespace

ChildLifeline::~ChildLifeline() { Close(); }

bool ChildLifeline::Open() {
    Close();
    int ends[2] = {-1, -1};
    if (::pipe(ends) != 0) {
        return false;
    }
    for (const int fd : ends) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    Read_ = ends[0];
    Write_ = ends[1];
    return true;
}

void ChildLifeline::Close() {
    for (int* fd : {&Read_, &Write_}) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
}

bool ChildLifeline::Guard(int readEnd, int writeEnd) {
    const pid_t command = ::getpid();
    const pid_t watchdog = ::fork();
    if (watchdog == 0) {
        ::close(writeEnd);
        Watch(readEnd, command);
    }
    return watchdog > 0;
}

}  // namespace roost::plugin
