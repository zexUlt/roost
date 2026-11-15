#include "core/pty.hpp"

#include <QProcess>
#include <QProcessEnvironment>
#include <QSocketNotifier>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#if defined(Q_OS_MACOS)
#include <util.h>
#else
#include <pty.h>
#endif

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>

namespace roost {

namespace {

// How much one wake-up reads before the event loop gets a turn.
constexpr qsizetype READ_LIMIT = 1024 * 1024;
constexpr std::size_t READ_CHUNK = 64 * 1024;

winsize WindowSize(PtySize size) {
    return winsize{.ws_row = size.Rows,
                   .ws_col = size.Columns,
                   .ws_xpixel = 0,
                   .ws_ypixel = 0};
}

// Moves fd above stdio and marks it close-on-exec. Qt points the child's
// stdin, stdout and stderr at /dev/null before the modifier runs, which would
// clobber a slave end numbered 0 to 2: roostd may run with stdio closed.
bool MoveAboveStdio(int& fd) {
    const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (moved == -1) {
        return false;
    }
    ::close(fd);
    fd = moved;
    return true;
}

bool SetNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    return flags != -1 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

QString Errno(const char* what) {
    return QStringLiteral("%1: %2").arg(
        QLatin1StringView(what), QString::fromLocal8Bit(std::strerror(errno)));
}

}  // namespace

Pty::Pty(PtyOptions options, QObject* parent)
    : QObject(parent), Options_(options), Process_(new QProcess(this)) {
    // A program that ignores the hang-up is killed after a while.
    KillTimer_.setSingleShot(true);
    connect(&KillTimer_, &QTimer::timeout, this, &Pty::Kill);
    connect(Process_, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus status) {
                KillTimer_.stop();
                // What it wrote before exiting is still in the PTY.
                Read();
                CloseMaster();
                emit Finished(
                    status == QProcess::CrashExit
                        ? QStringLiteral("was killed by a signal")
                        : QStringLiteral("exited with code %1").arg(exitCode));
            });
}

Pty::~Pty() {
    // No Finished from a Pty being destroyed.
    disconnect(Process_, nullptr, this, nullptr);
    CloseMaster();
    if (Process_->state() != QProcess::NotRunning) {
        Kill();
        Process_->waitForFinished();
    }
}

std::expected<void, QString> Pty::Start(const PtySpec& spec, PtySize size) {
    if (Master_ != -1 || Process_->state() != QProcess::NotRunning) {
        return std::unexpected(QStringLiteral("the PTY is already in use"));
    }
    int master = -1;
    int slave = -1;
    winsize windowSize = WindowSize(size);
    if (::openpty(&master, &slave, nullptr, nullptr, &windowSize) == -1) {
        return std::unexpected(Errno("openpty"));
    }
    // Neither end leaks into other children; the program gets the slave end
    // through dup2, which clears the flag on the copy.
    if (!MoveAboveStdio(master) || !MoveAboveStdio(slave) ||
        !SetNonBlocking(master)) {
        const QString error = Errno("fcntl");
        ::close(master);
        ::close(slave);
        return std::unexpected(error);
    }

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("TERM"),
                       QStringLiteral("xterm-256color"));
    for (const QString& entry : spec.Environment) {
        const qsizetype equals = entry.indexOf(u'=');
        if (equals > 0) {
            environment.insert(entry.first(equals), entry.sliced(equals + 1));
        }
    }
    Process_->setProcessEnvironment(environment);
    Process_->setProgram(spec.Program);
    Process_->setArguments(spec.Arguments);
    Process_->setWorkingDirectory(spec.WorkingDirectory);
    Process_->setStandardInputFile(QProcess::nullDevice());
    Process_->setStandardOutputFile(QProcess::nullDevice());
    Process_->setStandardErrorFile(QProcess::nullDevice());
    // The session is made here and not with CreateNewSession: Qt may apply
    // that after the modifier, and setsid() fails once the child already
    // leads a session. Async-signal-safe calls only: this runs between fork
    // and exec.
    Process_->setUnixProcessParameters(
        QProcess::UnixProcessFlag::ResetSignalHandlers);
    Process_->setChildProcessModifier([slave, process = Process_] {
        if (::setsid() == -1) {
            process->failChildProcessModifier("setsid", errno);
        }
        if (::ioctl(slave, TIOCSCTTY, 0) == -1) {
            process->failChildProcessModifier("TIOCSCTTY", errno);
        }
        for (int fd = 0; fd <= 2; ++fd) {
            if (::dup2(slave, fd) == -1) {
                process->failChildProcessModifier("dup2", errno);
            }
        }
        ::close(slave);
    });
    Process_->start();
    const bool started = Process_->waitForStarted();
    // The program holds its own copies of the slave end now.
    ::close(slave);
    if (!started) {
        ::close(master);
        return std::unexpected(Process_->errorString());
    }

    Master_ = master;
    Size_ = size;
    ReadNotifier_ = new QSocketNotifier(Master_, QSocketNotifier::Read, this);
    connect(ReadNotifier_, &QSocketNotifier::activated, this, &Pty::Read);
    WriteNotifier_ = new QSocketNotifier(Master_, QSocketNotifier::Write, this);
    WriteNotifier_->setEnabled(false);
    connect(WriteNotifier_, &QSocketNotifier::activated, this, &Pty::Flush);
    return {};
}

void Pty::Write(QByteArrayView bytes) {
    if (Master_ == -1 || bytes.isEmpty()) {
        return;
    }
    Unwritten_.append(bytes);
    Flush();
}

bool Pty::Resize(PtySize size) {
    const winsize windowSize = WindowSize(size);
    if (Master_ == -1 || ::ioctl(Master_, TIOCSWINSZ, &windowSize) == -1) {
        return false;
    }
    Size_ = size;
    return true;
}

void Pty::HangUp() {
    CloseMaster();
    if (Process_->state() != QProcess::NotRunning && !KillTimer_.isActive()) {
        KillTimer_.start(Options_.HangUpTimeout);
    }
}

bool Pty::IsRunning() const { return Process_->state() == QProcess::Running; }

qint64 Pty::ProcessId() const { return Process_->processId(); }

void Pty::Read() {
    if (Master_ == -1) {
        return;
    }
    // Read into a buffer of its own, so that what is emitted holds no more
    // memory than its bytes need: listeners may keep it.
    std::array<char, READ_CHUNK> buffer{};
    QByteArray bytes;
    while (bytes.size() < READ_LIMIT) {
        const ssize_t n = ::read(Master_, buffer.data(), buffer.size());
        if (n > 0) {
            bytes.append(buffer.data(), n);
            continue;
        }
        if (n == -1 && errno == EINTR) {
            continue;
        }
        // EAGAIN: nothing more for now. End of file, or EIO on Linux: the
        // program and everything it started closed the terminal; its exit
        // comes through the process.
        if (n == 0 || errno != EAGAIN) {
            ReadNotifier_->setEnabled(false);
        }
        break;
    }
    if (!bytes.isEmpty()) {
        emit Output(bytes);
    }
}

void Pty::Kill() {
    // The program leads its own session and process group: whatever it
    // started there and that ignored the hang-up goes too.
    if (const qint64 pid = Process_->processId(); pid > 0) {
        ::killpg(static_cast<pid_t>(pid), SIGKILL);
    }
    Process_->kill();
}

void Pty::Flush() {
    while (Master_ != -1 && !Unwritten_.isEmpty()) {
        const ssize_t n = ::write(Master_, Unwritten_.constData(),
                                  static_cast<size_t>(Unwritten_.size()));
        if (n > 0) {
            Unwritten_.remove(0, n);
            continue;
        }
        if (n == -1 && errno == EINTR) {
            continue;
        }
        if (n == -1 && errno == EAGAIN) {
            WriteNotifier_->setEnabled(true);
            return;
        }
        // The terminal is gone; so is the input meant for it.
        Unwritten_.clear();
    }
    if (WriteNotifier_ != nullptr) {
        WriteNotifier_->setEnabled(false);
    }
}

void Pty::CloseMaster() {
    if (Master_ == -1) {
        return;
    }
    // Notifiers go before the descriptor they watch.
    delete ReadNotifier_;
    ReadNotifier_ = nullptr;
    delete WriteNotifier_;
    WriteNotifier_ = nullptr;
    ::close(Master_);
    Master_ = -1;
    Unwritten_.clear();
}

}  // namespace roost
