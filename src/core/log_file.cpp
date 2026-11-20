#include "core/log_file.hpp"

#include <QFile>
#include <QFileInfo>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace roost {

namespace {

QString Failure(const QString& what, const QString& path) {
    return QStringLiteral("%1 %2: %3")
        .arg(what, path, QString::fromLocal8Bit(std::strerror(errno)));
}

}  // namespace

std::expected<void, QString> RotateLog(const QString& path) {
    const QString rotated = path + QStringLiteral(".1");
    // rename replaces the older file in one step.
    if (::rename(QFile::encodeName(path).constData(),
                 QFile::encodeName(rotated).constData()) != 0 &&
        errno != ENOENT) {
        return std::unexpected(Failure(QStringLiteral("cannot move"), path));
    }
    return {};
}

void OpenClosedStdio() {
    for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
        if (::fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
            continue;
        }
        // open takes the lowest free descriptor, which is fd: loop in order.
        const int opened = ::open("/dev/null", O_RDWR);
        if (opened > fd) {
            ::close(opened);
        }
    }
}

std::expected<void, QString> RedirectToLog(const QString& path) {
    // The working directory is not the one the caller meant, later on.
    const QString absolute = QFileInfo(path).absoluteFilePath();
    // The new file first: a failed open must leave the old log where it is.
    const QByteArray fresh =
        QFile::encodeName(absolute + QStringLiteral(".new"));
    const int fd = ::open(fresh.constData(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return std::unexpected(
            Failure(QStringLiteral("cannot open"), absolute));
    }
    auto rotated = RotateLog(absolute);
    if (rotated && ::rename(fresh.constData(),
                            QFile::encodeName(absolute).constData()) != 0) {
        rotated = std::unexpected(Failure(QStringLiteral("cannot move"),
                                          absolute + QStringLiteral(".new")));
    }
    if (!rotated) {
        const int savedErrno = errno;
        ::close(fd);
        ::unlink(fresh.constData());
        errno = savedErrno;
        return rotated;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    const bool done =
        ::dup2(fd, STDOUT_FILENO) >= 0 && ::dup2(fd, STDERR_FILENO) >= 0;
    const int savedErrno = errno;
    ::close(fd);
    errno = savedErrno;
    if (!done) {
        return std::unexpected(
            Failure(QStringLiteral("cannot redirect to"), absolute));
    }
    return {};
}

}  // namespace roost
