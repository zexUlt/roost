#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcessEnvironment>
#include <QString>

#include <chrono>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <optional>

namespace roost::notifications {

// One run of a user's command: `bash -c <Run>`.
struct CommandJob {
    // The name in the settings; the result is kept under it.
    QString Name;
    QString Run;
    // The event that caused the run: "blocked", "done", "failed" or "test".
    QString Event;
    QProcessEnvironment Environment;
    // What the command reads on its stdin, which is closed after it.
    QByteArray Input;
};

// How a run ended. Exactly one of ExitCode and Error is set.
struct CommandResult {
    QString Name;
    QString Event;
    std::optional<int> ExitCode;
    // "timeout", "failed to start" or "crashed" (a signal that was not ours);
    // empty if the command exited.
    QString Error;
    // The last bytes of the stderr of the command.
    QString Stderr;
    // Milliseconds since the epoch.
    qint64 At = 0;
};

struct RunnerOptions {
    // A command that runs longer, with its whole process group, is killed.
    std::chrono::milliseconds Timeout{10'000};
    // The rest wait, in order, in a queue; a new run beyond it is dropped.
    int MaxRunning = 4;
    int MaxQueued = 64;
    // The part of stderr kept.
    qsizetype StderrBytes = 2000;
};

// Runs the commands of the user (docs/design/05-plugins.md, M8): each in a
// session and process group of its own, from the home directory, with no stdout
// and the job's input on stdin. Deleting the runner kills what still runs.
class CommandRunner : public QObject {
    Q_OBJECT

public:
    using Done = std::function<void(const CommandResult&)>;

    // `done` is called from the event loop, once for each run that started.
    CommandRunner(RunnerOptions options, Done done, QObject* parent = nullptr);
    ~CommandRunner() override;

    CommandRunner(const CommandRunner&) = delete;
    CommandRunner& operator=(const CommandRunner&) = delete;

    // Starts the job, or queues it if MaxRunning run. False, with a warning
    // on stderr, if the queue is full and the job is dropped.
    bool Submit(CommandJob job);

    int RunningCount() const;
    int QueuedCount() const;

private:
    class Run;

    void Launch(CommandJob job);
    void OnEnded(Run* run, const CommandResult& result);

    RunnerOptions Options_;
    Done Done_;
    std::list<Run*> Running_;
    std::deque<CommandJob> Queue_;
};

}  // namespace roost::notifications
