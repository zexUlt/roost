#pragma once

#include <sys/types.h>

namespace roost::plugin {

// Makes a child process die with the process that started it, however that
// one ends. There is no PDEATHSIG on macOS, so the way is the one of
// tests/helpers/process_lifeline: a pipe whose write end only the parent
// holds, and a watchdog, a fork of the child made before it execs, that reads
// the other end and kills the child's process group when the pipe reaches EOF.
//
// Use: Open(), then in the child process modifier of the QProcess, after
// setsid(), Guard(ReadEnd(), WriteEnd()). Closing the lifeline (Close(), the
// destructor, or the death of the parent) makes a watchdog whose child is
// still running kill the group; one whose child is gone ends quietly.
class ChildLifeline {
public:
    ChildLifeline() = default;
    ~ChildLifeline();

    ChildLifeline(const ChildLifeline&) = delete;
    ChildLifeline& operator=(const ChildLifeline&) = delete;

    // A new pipe, both ends close-on-exec: the command and what it starts do
    // not hold them. False if there is no pipe.
    bool Open();
    void Close();

    int ReadEnd() const { return Read_; }
    int WriteEnd() const { return Write_; }

    // In the child, between fork and exec, after the child became the leader
    // of its session and group. Forks the watchdog, which stays in the group.
    // False, with errno set, if the fork failed. Async-signal-safe calls only.
    static bool Guard(int readEnd, int writeEnd);

private:
    int Read_ = -1;
    int Write_ = -1;
};

}  // namespace roost::plugin
