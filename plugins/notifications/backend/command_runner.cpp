#include "notifications/backend/command_runner.hpp"

#include <QDateTime>
#include <QProcess>
#include <QTimer>

#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

#include "common/backend_process.hpp"
#include "common/child_lifeline.hpp"
#include "notifications/backend/diagnostics.hpp"

namespace roost::notifications {

// One process. Ends exactly once, whichever of the exit, the failure to start
// and the timeout comes first.
class CommandRunner::Run : public QObject {
public:
    Run(CommandJob job, const RunnerOptions& options, CommandRunner& runner)
        : QObject(&runner),
          Job_(std::move(job)),
          StderrBytes_(options.StderrBytes),
          Runner_(runner),
          Process_(this),
          Timer_(this) {
        Process_.setProgram(QStringLiteral("bash"));
        Process_.setArguments({QStringLiteral("-c"), Job_.Run});
        Process_.setProcessEnvironment(Job_.Environment);
        // roostd starts the backend in the plugin directory; a command runs
        // from the home directory, as the commands of the other plugins do.
        if (const QString home = plugin::HomeDirectory(); !home.isEmpty()) {
            Process_.setWorkingDirectory(home);
        }
        Process_.setStandardOutputFile(QProcess::nullDevice());
        // Default signal handlers, as roostd gives its backends. The session
        // is made in the modifier of Start, not with CreateNewSession.
        Process_.setUnixProcessParameters(
            QProcess::UnixProcessFlag::ResetSignalHandlers);

        connect(&Process_, &QProcess::started, this, [this] {
            Process_.write(Job_.Input);
            Process_.closeWriteChannel();
        });
        connect(&Process_, &QProcess::readyReadStandardError, this,
                [this] { AppendStderr(); });
        connect(&Process_, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError error) {
                    if (error == QProcess::FailedToStart) {
                        End(std::nullopt, QStringLiteral("failed to start"),
                            Process_.errorString());
                    }
                });
        connect(&Process_, &QProcess::finished, this,
                [this](int exitCode, QProcess::ExitStatus status) {
                    AppendStderr();
                    if (TimedOut_) {
                        End(std::nullopt, QStringLiteral("timeout"),
                            QString::fromUtf8(Stderr_));
                    } else if (status == QProcess::CrashExit) {
                        End(std::nullopt, QStringLiteral("crashed"),
                            QString::fromUtf8(Stderr_));
                    } else {
                        End(exitCode, QString(), QString::fromUtf8(Stderr_));
                    }
                });

        Timer_.setSingleShot(true);
        connect(&Timer_, &QTimer::timeout, this, [this] {
            // A process that has exited but whose end the loop has not yet
            // delivered did not time out: take its end now, with its exit
            // code, and do not kill a group that may be a new process's.
            if (Process_.state() == QProcess::Running &&
                Process_.waitForFinished(0)) {
                return;
            }
            if (Ended_) {
                return;
            }
            TimedOut_ = true;
            const auto group = static_cast<pid_t>(Process_.processId());
            if (group > 0) {
                ::killpg(group, SIGKILL);
            }
            Process_.kill();
        });
        Timeout_ = options.Timeout;
    }

    void Start() {
        // A session of its own, so that the group is the process and all it
        // forked. Made here and not with CreateNewSession: Qt may apply that
        // after the modifier, and the watchdog has to be in the group, whose
        // id is the pid of the command. Async-signal-safe calls only: this
        // runs between fork and exec. The command dies with the backend,
        // however that ends: the watchdog kills its group when the backend
        // closes the write end of the lifeline, which the system does too
        // when the backend is killed.
        if (!Lifeline_.Open()) {
            End(std::nullopt, QStringLiteral("failed to start"),
                QStringLiteral("no pipe for the lifeline of the command"));
            return;
        }
        Process_.setChildProcessModifier([process = &Process_,
                                          readEnd = Lifeline_.ReadEnd(),
                                          writeEnd = Lifeline_.WriteEnd()] {
            if (::setsid() == -1) {
                process->failChildProcessModifier("setsid", errno);
            }
            if (!plugin::ChildLifeline::Guard(readEnd, writeEnd)) {
                // Without the watchdog a killed backend would leave the
                // command behind, so the start fails instead.
                process->failChildProcessModifier("fork of the watchdog",
                                                  errno);
            }
            // The watchdog has its own copy. This one would keep the pipe
            // open for as long as the command has not exec'd, and then the
            // lifeline would not end with the backend.
            ::close(writeEnd);
        });
        Timer_.start(Timeout_);
        Process_.start();
    }

    ~Run() override {
        // Gone while it runs, with the runner: nothing is left behind.
        if (!Ended_ && Process_.state() != QProcess::NotRunning) {
            Process_.disconnect(this);
            plugin::StopRun(Process_);
        }
    }

    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

private:
    // Keeps the last StderrBytes_ bytes, starting at a character: a cut inside
    // a UTF-8 sequence would show as a replacement character.
    void AppendStderr() {
        Stderr_.append(Process_.readAllStandardError());
        if (Stderr_.size() <= StderrBytes_) {
            return;
        }
        qsizetype from = Stderr_.size() - StderrBytes_;
        while (from < Stderr_.size() &&
               (static_cast<unsigned char>(Stderr_[from]) & 0xC0) == 0x80) {
            ++from;
        }
        Stderr_.remove(0, from);
    }

    void End(std::optional<int> exitCode, const QString& error,
             const QString& stderrText) {
        if (std::exchange(Ended_, true)) {
            return;
        }
        Timer_.stop();
        Process_.disconnect(this);
        Lifeline_.Close();
        CommandResult result{.Name = Job_.Name,
                             .Event = Job_.Event,
                             .ExitCode = exitCode,
                             .Error = error,
                             .Stderr = stderrText,
                             .At = QDateTime::currentMSecsSinceEpoch()};
        Runner_.OnEnded(this, result);
    }

    CommandJob Job_;
    qsizetype StderrBytes_;
    CommandRunner& Runner_;
    QProcess Process_;
    QTimer Timer_;
    plugin::ChildLifeline Lifeline_;
    std::chrono::milliseconds Timeout_{};
    QByteArray Stderr_;
    bool TimedOut_ = false;
    bool Ended_ = false;
};

CommandRunner::CommandRunner(RunnerOptions options, Done done, QObject* parent)
    : QObject(parent), Options_(options), Done_(std::move(done)) {}

CommandRunner::~CommandRunner() {
    Queue_.clear();
    // The runs are children of the runner and are deleted with it.
}

bool CommandRunner::Submit(CommandJob job) {
    if (static_cast<int>(Running_.size()) < Options_.MaxRunning) {
        Launch(std::move(job));
        return true;
    }
    if (static_cast<int>(Queue_.size()) >= Options_.MaxQueued) {
        Warn(QStringLiteral("%1 commands are waiting: the run of %2 for %3 is "
                            "dropped")
                 .arg(Queue_.size())
                 .arg(job.Name, job.Event));
        return false;
    }
    Queue_.push_back(std::move(job));
    return true;
}

int CommandRunner::RunningCount() const {
    return static_cast<int>(Running_.size());
}

int CommandRunner::QueuedCount() const {
    return static_cast<int>(Queue_.size());
}

void CommandRunner::Launch(CommandJob job) {
    auto* run = new Run(std::move(job), Options_, *this);
    Running_.push_back(run);
    run->Start();
}

void CommandRunner::OnEnded(Run* run, const CommandResult& result) {
    Running_.remove(run);
    run->deleteLater();
    if (!Queue_.empty()) {
        CommandJob next = std::move(Queue_.front());
        Queue_.pop_front();
        Launch(std::move(next));
    }
    Done_(result);
}

}  // namespace roost::notifications
