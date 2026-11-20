#include "roost/window_log.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSysInfo>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "core/log_file.hpp"
#include "core/version.hpp"
#include "roost/daemon_launcher.hpp"

namespace roost {

namespace {

Q_LOGGING_CATEGORY(WindowCategory, "roost.window", QtInfoMsg)

constexpr mode_t LOG_MODE = 0600;
constexpr mode_t DIRECTORY_MODE = 0700;

// What the handler uses. Set before the handler is installed and cleared
// after it is removed, so a message in between sees one or the other.
std::atomic<WindowLogFile*> ACTIVE_FILE{nullptr};
std::atomic<QtMessageHandler> PREVIOUS_HANDLER{nullptr};

char LevelLetter(QtMsgType type) {
    switch (type) {
        case QtDebugMsg:
            return 'D';
        case QtInfoMsg:
            return 'I';
        case QtWarningMsg:
            return 'W';
        case QtCriticalMsg:
            return 'C';
        case QtFatalMsg:
            return 'F';
    }
    return '?';
}

void Handle(QtMsgType type, const QMessageLogContext& context,
            const QString& message) {
    if (const WindowLogFile* const file =
            ACTIVE_FILE.load(std::memory_order_acquire)) {
        file->Write(FormatWindowLogLine(type, context, message,
                                        QDateTime::currentDateTime(),
                                        QCoreApplication::applicationPid()));
    }
    if (const QtMessageHandler previous = PREVIOUS_HANDLER.load()) {
        previous(type, context, message);
        return;
    }
    // Qt's own handler is not public: this is what it prints to stderr.
    const QByteArray line =
        qFormatLogMessage(type, context, message).toLocal8Bit();
    std::fprintf(stderr, "%s\n", line.constData());
    std::fflush(stderr);
}

const char* GraphicsApiName(QSGRendererInterface::GraphicsApi api) {
    switch (api) {
        case QSGRendererInterface::Unknown:
            return "unknown";
        case QSGRendererInterface::Software:
            return "software";
        case QSGRendererInterface::OpenGL:
            return "opengl";
        case QSGRendererInterface::Direct3D11:
            return "direct3d11";
        case QSGRendererInterface::Vulkan:
            return "vulkan";
        case QSGRendererInterface::Metal:
            return "metal";
        case QSGRendererInterface::Null:
            return "null";
        case QSGRendererInterface::Direct3D12:
            return "direct3d12";
        default:
            return "other";
    }
}

}  // namespace

std::expected<WindowLogFile, QString> WindowLogFile::Open(const QString& path,
                                                          qint64 rotateAbove) {
    const QByteArray encoded = QFile::encodeName(path);
    // A window that comes up before roostd has made the directory.
    const QByteArray directory =
        QFile::encodeName(QFileInfo(path).absolutePath());
    if (::mkdir(directory.constData(), DIRECTORY_MODE) != 0 &&
        errno != EEXIST) {
        return std::unexpected(
            QStringLiteral("cannot make %1: %2")
                .arg(QString::fromLocal8Bit(directory),
                     QString::fromLocal8Bit(std::strerror(errno))));
    }
    if (QFileInfo(path).size() > rotateAbove) {
        // Not fatal: the log then just keeps growing.
        (void)RotateLog(path);
    }
    const int fd = ::open(
        encoded.constData(),
        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, LOG_MODE);
    if (fd < 0) {
        return std::unexpected(
            QStringLiteral("cannot open %1: %2")
                .arg(path, QString::fromLocal8Bit(std::strerror(errno))));
    }
    return WindowLogFile(fd);
}

WindowLogFile::WindowLogFile(WindowLogFile&& other) noexcept
    : Fd_(std::exchange(other.Fd_, -1)) {}

WindowLogFile& WindowLogFile::operator=(WindowLogFile&& other) noexcept {
    if (this != &other) {
        if (Fd_ >= 0) {
            ::close(Fd_);
        }
        Fd_ = std::exchange(other.Fd_, -1);
    }
    return *this;
}

WindowLogFile::~WindowLogFile() {
    if (Fd_ >= 0) {
        ::close(Fd_);
    }
}

void WindowLogFile::Write(const QByteArray& line) const {
    if (Fd_ >= 0) {
        (void)::write(Fd_, line.constData(), static_cast<size_t>(line.size()));
    }
}

namespace {

// One record is one line: a backslash, a newline and a carriage return in the
// text are written as \\, \n and \r.
QByteArray EscapeLine(const QByteArray& text) {
    QByteArray escaped;
    escaped.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '\\':
                escaped += "\\\\";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            default:
                escaped += c;
        }
    }
    return escaped;
}

}  // namespace

QByteArray FormatWindowLogLine(QtMsgType type,
                               const QMessageLogContext& context,
                               const QString& message, const QDateTime& time,
                               qint64 pid) {
    QByteArray line = time.toString(Qt::ISODateWithMs).toUtf8();
    line += " pid=" + QByteArray::number(pid) + ' ' + LevelLetter(type) + ' ';
    if (context.category != nullptr &&
        std::strcmp(context.category, "default") != 0) {
        line += context.category;
        line += ": ";
    }
    line += EscapeLine(message.toUtf8());
    line += '\n';
    return line;
}

QString WindowLogPath(const QString& socketPath) {
    return DaemonLauncher::LogDirectory(socketPath) +
           QStringLiteral("/roost.log");
}

bool InstallWindowLog(const QString& path, qint64 rotateAbove) {
    auto opened = WindowLogFile::Open(path, rotateAbove);
    if (!opened) {
        qWarning("roost: no window log: %s", qPrintable(opened.error()));
        return false;
    }
    UninstallWindowLog();
    ACTIVE_FILE.store(new WindowLogFile(std::move(*opened)),
                      std::memory_order_release);
    PREVIOUS_HANDLER.store(qInstallMessageHandler(&Handle));
    return true;
}

void UninstallWindowLog() {
    WindowLogFile* const file = ACTIVE_FILE.exchange(nullptr);
    if (file == nullptr) {
        return;
    }
    qInstallMessageHandler(PREVIOUS_HANDLER.exchange(nullptr));
    delete file;
}

void LogWindowStartup() {
    qCInfo(WindowCategory, "roost %s, Qt %s, %s, %s, platform %s",
           VersionString().c_str(), qVersion(),
           qPrintable(QSysInfo::prettyProductName()),
           qPrintable(QSysInfo::currentCpuArchitecture()),
           qPrintable(QGuiApplication::platformName()));
}

void LogGraphicsApiWhenReady(QQuickWindow* window) {
    if (window == nullptr) {
        return;
    }
    // From the render thread; queued to ours, where the interface is read.
    QObject::connect(
        window, &QQuickWindow::sceneGraphInitialized, window,
        [window] {
            if (const QSGRendererInterface* const renderer =
                    window->rendererInterface()) {
                qCInfo(WindowCategory, "graphics API %s",
                       GraphicsApiName(renderer->graphicsApi()));
            }
        },
        Qt::SingleShotConnection);
}

}  // namespace roost
