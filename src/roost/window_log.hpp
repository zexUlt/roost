#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QtLogging>
#include <QtTypes>

#include <expected>

class QQuickWindow;

namespace roost {

// The file of the window's log: roost.log, next to roostd.log. Lines are
// appended with one write each, so that windows running at the same time do
// not cut each other's lines.
class WindowLogFile {
public:
    // The file is moved to `<path>.1` first if it is larger than this.
    static constexpr qint64 ROTATE_ABOVE_BYTES = 1024 * 1024;

    // Rotates `path` if it is larger than `rotateAbove`, then opens it for
    // appending (O_APPEND, mode 0600). A missing directory is made (0700, as
    // roostd makes it). A failed rotation is not an error: the log just grows.
    static std::expected<WindowLogFile, QString> Open(
        const QString& path, qint64 rotateAbove = ROTATE_ABOVE_BYTES);

    WindowLogFile(WindowLogFile&& other) noexcept;
    WindowLogFile& operator=(WindowLogFile&& other) noexcept;
    WindowLogFile(const WindowLogFile&) = delete;
    WindowLogFile& operator=(const WindowLogFile&) = delete;
    ~WindowLogFile();

    // One write() of the line. Any thread. A short or failed write is dropped:
    // logging must not fail the window.
    void Write(const QByteArray& line) const;

private:
    explicit WindowLogFile(int fd) : Fd_(fd) {}

    int Fd_ = -1;
};

// "2026-11-20T14:05:01.123 pid=12345 W roost.terminal: message\n". The level
// is D, I, W, C or F; the category is left out when it is "default".
QByteArray FormatWindowLogLine(QtMsgType type,
                               const QMessageLogContext& context,
                               const QString& message, const QDateTime& time,
                               qint64 pid);

// roost.log in the directory of the socket (roost::protocol::SocketPath()).
QString WindowLogPath(const QString& socketPath);

// Installs a Qt message handler that writes every message to the log and then
// passes it on to the handler that was there (stderr by default). Call once,
// right after QGuiApplication is created. If the file cannot be opened, warns
// once on stderr and installs nothing. Returns whether it logs to a file.
bool InstallWindowLog(const QString& path,
                      qint64 rotateAbove = WindowLogFile::ROTATE_ABOVE_BYTES);

// Puts the previous handler back and closes the file. For tests; no other
// thread may log meanwhile.
void UninstallWindowLog();

// One info line: roost version, Qt runtime, OS, CPU architecture, QPA
// platform. Needs the QGuiApplication.
void LogWindowStartup();

// Adds the graphics API of `window` to the log once its scene graph is up.
void LogGraphicsApiWhenReady(QQuickWindow* window);

}  // namespace roost
