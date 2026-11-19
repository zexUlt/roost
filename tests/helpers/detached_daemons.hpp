#pragma once

#include <QString>
#include <QStringList>
#include <QtTypes>

namespace roost::testing {

// Stops the roostd that the window under test started by itself. Such a daemon
// is detached into a session of its own, so it outlives the test unless
// something kills it: this does, in two ways.
//
// The pid files are given at construction (`<socket>.lock` of roostd, the pid
// file of a fake claude): files whose first line is a pid. KillAll reads them
// and stops what they name (SIGTERM, then SIGKILL after a wait). And a
// watcher, forked at construction and in a session of its own, does the same
// if this process dies without running destructors (abort, crash, a kill by
// ctest): it reads the files then. A pipe tells it that the process lives; the
// destructor sends it a byte, so a normal end leaves the watcher nothing to do.
class DetachedDaemons {
public:
    explicit DetachedDaemons(const QStringList& pidFiles);
    // KillAll, then dismisses the watcher.
    ~DetachedDaemons();

    DetachedDaemons(const DetachedDaemons&) = delete;
    DetachedDaemons& operator=(const DetachedDaemons&) = delete;

    // Stops the processes of the pid files and waits for them to go.
    void KillAll();

    // The pid on the first line of the file (QLockFile writes it so), or 0.
    static qint64 PidOf(const QString& pidFile);
    static bool IsAlive(qint64 pid);

private:
    QStringList PidFiles_;
    int Lifeline_ = -1;
};

}  // namespace roost::testing
