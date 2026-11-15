#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <expected>

class QProcess;
class QSocketNotifier;

namespace roost {

// What runs in a PTY: an agent adapter's attach command (design.md §7).
struct PtySpec {
    QString Program;
    QStringList Arguments;
    // Empty: the working directory of roostd.
    QString WorkingDirectory;
    // "NAME=value" entries on top of roostd's environment. TERM is
    // xterm-256color unless set here.
    QStringList Environment;
};

struct PtySize {
    quint16 Columns = 80;
    quint16 Rows = 24;

    bool operator==(const PtySize&) const = default;
};

struct PtyOptions {
    // How long a program has to exit after a hang-up before it is killed.
    std::chrono::milliseconds HangUpTimeout{1'000};
};

// A program in a pseudo-terminal of its own (design.md §3): it runs in a new
// session with the PTY as its controlling terminal and stdin, stdout and
// stderr. Output is read as it comes. Input is buffered without a limit until
// the PTY takes it, and goes through the terminal's line discipline as the
// program has set it: before the program switches to raw mode the driver
// echoes it and turns ^C into SIGINT, as in any terminal.
//
// Finished is emitted once, after the program exited and the output it wrote
// before that was emitted. Signals run inside the Pty's own code: a Pty is
// not deleted from them, only through deleteLater().
class Pty : public QObject {
    Q_OBJECT

public:
    explicit Pty(PtyOptions options = {}, QObject* parent = nullptr);
    // Kills a program that still runs, with its process group, without
    // emitting Finished.
    ~Pty() override;

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    // Once per Pty. Returns when the program has started or failed to.
    std::expected<void, QString> Start(const PtySpec& spec, PtySize size);

    // Dropped once the PTY is closed.
    void Write(QByteArrayView bytes);
    // TIOCSWINSZ: the program gets SIGWINCH. False once the PTY is closed.
    bool Resize(PtySize size);
    // Closes the PTY, which sends the program SIGHUP, and kills its process
    // group if it has not exited within the hang-up timeout. Output from now
    // on is lost.
    void HangUp();

    bool IsRunning() const;
    qint64 ProcessId() const;
    PtySize Size() const { return Size_; }

signals:
    void Output(const QByteArray& bytes);
    // "exited with code N" or "was killed by a signal".
    void Finished(const QString& reason);

private:
    void Read();
    void Flush();
    void Kill();
    void CloseMaster();

    PtyOptions Options_;
    QProcess* Process_ = nullptr;
    int Master_ = -1;
    QSocketNotifier* ReadNotifier_ = nullptr;
    QSocketNotifier* WriteNotifier_ = nullptr;
    QByteArray Unwritten_;
    QTimer KillTimer_;
    PtySize Size_;
};

}  // namespace roost
