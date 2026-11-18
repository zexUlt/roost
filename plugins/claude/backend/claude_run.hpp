#pragma once

#include <QByteArray>
#include <QFuture>
#include <QObject>
#include <QProcess>
#include <QPromise>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <expected>

namespace roost::claude {

struct RunRequest {
    QString Program;
    QStringList Args;
    // Empty: the working directory of the adapter.
    QString WorkingDirectory;
    // A run that takes longer is killed and counts as failed.
    std::chrono::milliseconds Timeout{60'000};
};

// What a run that ended on its own printed. A non-zero ExitCode is not an
// error of the run: the caller decides what it means.
struct RunOutput {
    int ExitCode = 0;
    QByteArray StandardOutput;
    QByteArray StandardError;
};

// A run that did not end with an exit code carries why, in one line.
using RunResult = std::expected<RunOutput, QString>;

// Kills a run still going, with its process group, so that what it started
// does not outlive it. The group is signalled again once the leader is reaped,
// for a fork that raced with the first signal; the group id stays taken while
// it has a member. A run that has no process (it failed to start) signals no
// group: pid 0 would be the group of the adapter.
void StopRun(QProcess& process);

// Why a run with a non-zero exit failed, in one line: the first line of stderr
// that is not a warning, else the same of stdout, else the exit code.
QString FailureReason(const RunOutput& output, const QString& verb);

// One run of `claude` to the end, in a group of its own with default signal
// handlers and no stdin, as roostd runs its backends. Start it once. Deleting
// it while it runs kills it and fails the future.
class ClaudeRun : public QObject {
    Q_OBJECT

public:
    explicit ClaudeRun(RunRequest request, QObject* parent = nullptr);
    ~ClaudeRun() override;

    ClaudeRun(const ClaudeRun&) = delete;
    ClaudeRun& operator=(const ClaudeRun&) = delete;

    // The run deletes itself (deleteLater) once it has ended.
    QFuture<RunResult> Start();

private:
    // Exactly one call ends the run; the process is gone after it.
    void Finish(RunResult result);

    RunRequest Request_;
    QPromise<RunResult> Promise_;
    QProcess Process_;
    QTimer TimeoutTimer_;
    bool Started_ = false;
    bool Finished_ = false;
};

}  // namespace roost::claude
