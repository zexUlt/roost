#pragma once

#include <QProcess>

namespace roost::testing {

// For a child that would otherwise inherit the standard error of the test.
// ctest reads it until EOF, and a roostd or a backend of it that survives the
// test would hold it, so ctest would hang and the output of the test would
// never be printed. The standard error of the child goes through the test
// process instead, which passes it on to its own once the event loop runs; the
// child holds a pipe of the test, not the one of ctest. Call before
// QProcess::start.
void RelayStandardError(QProcess& process);

// Ties a child of the test to the test process, for a child that outlives it
// otherwise (roostd). If the test process dies without running destructors
// (abort, crash, an ASan report, a kill by ctest), a watcher next to the child
// notices it and stops the child (SIGTERM, then SIGKILL).
//
// Does not touch the standard streams of the child: see RelayStandardError.
// One object per QProcess: Attach before QProcess::start, Release after the
// child has left (a new Attach does it by itself). The object holds the write
// end of the pipe the watcher reads, so it must live as long as the child is
// meant to.
class ProcessLifeline {
public:
    ProcessLifeline() = default;
    ~ProcessLifeline();

    ProcessLifeline(const ProcessLifeline&) = delete;
    ProcessLifeline& operator=(const ProcessLifeline&) = delete;

    // False if there is no pipe: the child must not be started then.
    bool Attach(QProcess& process);
    // Lets the watcher go: it ends, if the child has not left yet it stops it.
    void Release();

private:
    // The read end is the watcher's; this process keeps a copy only to close it
    // together with the write end.
    int Read_ = -1;
    int Write_ = -1;
};

}  // namespace roost::testing
