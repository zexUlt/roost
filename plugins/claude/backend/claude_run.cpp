#include "claude/backend/claude_run.hpp"

#include <QRegularExpression>
#include <QString>

#include <signal.h>

#include <utility>

namespace roost::claude {

namespace {

// A line that is a warning: "warn:" or "warning:" first, in any case. A line
// that only mentions the word, as in a session name, is not.
bool IsWarning(const QString& line) {
    static const QRegularExpression WARNING(
        QStringLiteral("^warn(ing)?\\s*:"),
        QRegularExpression::CaseInsensitiveOption);
    return WARNING.match(line).hasMatch();
}

// The first line of the text that has words and is no warning, trimmed.
QString FirstReason(const QByteArray& text) {
    const QStringList lines = QString::fromUtf8(text).split(u'\n');
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (!line.isEmpty() && !IsWarning(line)) {
            return line;
        }
    }
    return {};
}

}  // namespace

void StopRun(QProcess& process) {
    if (process.state() == QProcess::NotRunning) {
        return;
    }
    // A run that failed to start (chdir failed, say) reports it while the
    // state is still Starting, with no process: the pid is 0, and killpg(0)
    // is the group of the adapter itself. There is nothing to signal or to
    // wait for, and waitForFinished would block for its whole timeout.
    const auto group = static_cast<pid_t>(process.processId());
    if (group <= 0) {
        return;
    }
    ::killpg(group, SIGKILL);
    process.kill();
    process.waitForFinished();
    // A fork that raced with the first signal may have joined the group
    // after it was delivered; the group lives on while it has a member.
    ::killpg(group, SIGKILL);
}

QString FailureReason(const RunOutput& output, const QString& verb) {
    QString reason = FirstReason(output.StandardError);
    if (reason.isEmpty()) {
        reason = FirstReason(output.StandardOutput);
    }
    if (reason.isEmpty()) {
        reason = QStringLiteral("claude %1 exited with code %2")
                     .arg(verb)
                     .arg(output.ExitCode);
    }
    return reason;
}

ClaudeRun::ClaudeRun(RunRequest request, QObject* parent)
    : QObject(parent), Request_(std::move(request)) {
    TimeoutTimer_.setSingleShot(true);
}

ClaudeRun::~ClaudeRun() {
    if (Started_ && !Finished_) {
        Process_.disconnect(this);
        StopRun(Process_);
        Finished_ = true;
        Promise_.addResult(std::unexpected(
            QStringLiteral("the adapter stopped before claude %1 finished")
                .arg(Request_.Args.value(0))));
        Promise_.finish();
    }
}

QFuture<RunResult> ClaudeRun::Start() {
    Q_ASSERT(!Started_);
    Started_ = true;
    const QString verb = Request_.Args.value(0);
    QFuture<RunResult> future = Promise_.future();
    Promise_.start();

    Process_.setProgram(Request_.Program);
    Process_.setArguments(Request_.Args);
    if (!Request_.WorkingDirectory.isEmpty()) {
        Process_.setWorkingDirectory(Request_.WorkingDirectory);
    }
    Process_.setStandardInputFile(QProcess::nullDevice());
    // A group of its own to kill on a timeout, and default signal handlers.
    Process_.setUnixProcessParameters(
        QProcess::UnixProcessFlag::CreateNewSession |
        QProcess::UnixProcessFlag::ResetSignalHandlers);

    connect(&Process_, &QProcess::errorOccurred, this,
            [this, verb](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    Finish(std::unexpected(
                        QStringLiteral("cannot run claude %1: %2")
                            .arg(verb, Process_.errorString())));
                }
            });
    connect(
        &Process_, &QProcess::finished, this,
        [this, verb](int code, QProcess::ExitStatus status) {
            if (status != QProcess::NormalExit) {
                Finish(std::unexpected(
                    QStringLiteral("claude %1 crashed").arg(verb)));
                return;
            }
            Finish(RunOutput{.ExitCode = code,
                             .StandardOutput = Process_.readAllStandardOutput(),
                             .StandardError = Process_.readAllStandardError()});
        });
    connect(&TimeoutTimer_, &QTimer::timeout, this, [this, verb] {
        Finish(
            std::unexpected(QStringLiteral("claude %1 did not finish in %2 ms")
                                .arg(verb)
                                .arg(Request_.Timeout.count())));
    });
    TimeoutTimer_.start(Request_.Timeout);
    Process_.start();
    return future;
}

void ClaudeRun::Finish(RunResult result) {
    if (Finished_) {
        return;
    }
    Finished_ = true;
    TimeoutTimer_.stop();
    Process_.disconnect(this);
    StopRun(Process_);
    Promise_.addResult(std::move(result));
    Promise_.finish();
    deleteLater();
}

}  // namespace roost::claude
