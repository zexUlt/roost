#include "roostd/log_file.hpp"

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

std::expected<void, QString> RedirectToLog(const QString& path) {
    // The working directory is not the one the caller meant, later on.
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (auto rotated = RotateLog(absolute); !rotated) {
        return rotated;
    }
    const int fd = ::open(QFile::encodeName(absolute).constData(),
                          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) {
        return std::unexpected(
            Failure(QStringLiteral("cannot open"), absolute));
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
