#include "core/appimage_install.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace roost {

namespace {

constexpr mode_t MIN_MODE = 0755;
constexpr mode_t MODE_MASK = 0777;
constexpr std::size_t COPY_BLOCK = 1U << 16;

QString ErrnoText(int error) { return QString::fromLocal8Bit(strerror(error)); }

// The text a failure of the directory gets: what the user can do by hand.
QString ManualSteps() {
    return QStringLiteral(
        "; download the AppImage from the release page and replace the file "
        "by hand");
}

// Removes the temporary file when the install does not reach the rename.
class TemporaryFile {
public:
    TemporaryFile() = default;
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    ~TemporaryFile() { Discard(); }

    int Fd = -1;
    QByteArray Path;

    void Discard() {
        if (Fd >= 0) {
            ::close(Fd);
            Fd = -1;
        }
        if (!Path.isEmpty()) {
            ::unlink(Path.constData());
            Path.clear();
        }
    }
    void Release() { Path.clear(); }
};

// Copies and hashes what it copies: the digest is of the bytes written.
std::expected<void, QString> Copy(int from, const QString& fromPath, int to,
                                  const QString& toPath,
                                  QCryptographicHash& hash) {
    QByteArray block(static_cast<qsizetype>(COPY_BLOCK), Qt::Uninitialized);
    for (;;) {
        const ssize_t got = ::read(from, block.data(), COPY_BLOCK);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(QStringLiteral("cannot read %1: %2")
                                       .arg(fromPath, ErrnoText(errno)));
        }
        if (got == 0) {
            return {};
        }
        hash.addData(QByteArrayView(block.constData(), got));
        for (ssize_t done = 0; done < got;) {
            const ssize_t put = ::write(to, block.constData() + done,
                                        static_cast<std::size_t>(got - done));
            if (put < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(QStringLiteral("cannot write %1: %2")
                                           .arg(toPath, ErrnoText(errno)));
            }
            done += put;
        }
    }
}

}  // namespace

std::optional<QString> AppImagePath(const QString& appImage,
                                    const QString& appDir,
                                    const QString& executableDir) {
    if (appImage.isEmpty() || appDir.isEmpty() ||
        !QFileInfo(appImage).isFile()) {
        return std::nullopt;
    }
    const QString root = QDir::cleanPath(appDir);
    const QString executable = QDir::cleanPath(executableDir);
    // The root is the prefix of every path; "/" + "/" would match none.
    const QString prefix =
        root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    if (executable != root && !executable.startsWith(prefix)) {
        return std::nullopt;
    }
    return appImage;
}

std::optional<QString> RunningAppImagePath() {
    return AppImagePath(qEnvironmentVariable("APPIMAGE"),
                        qEnvironmentVariable("APPDIR"),
                        QCoreApplication::applicationDirPath());
}

std::expected<void, QString> ReplaceAppImage(const QString& appImage,
                                             const QString& package,
                                             const QByteArray& digest) {
    if (!QFileInfo(package).isFile()) {
        return std::unexpected(
            QStringLiteral("the downloaded package %1 is missing")
                .arg(package));
    }
    // The link stays, its target is replaced.
    const QString target = QFileInfo(appImage).canonicalFilePath();
    if (target.isEmpty() || !QFileInfo(target).isFile()) {
        return std::unexpected(
            QStringLiteral("the AppImage %1 is missing").arg(appImage));
    }

    const QByteArray targetPath = QFile::encodeName(target);
    struct stat info{};
    if (::stat(targetPath.constData(), &info) != 0) {
        return std::unexpected(
            QStringLiteral("cannot read %1: %2").arg(target, ErrnoText(errno)));
    }
    const mode_t mode = (info.st_mode & MODE_MASK) | MIN_MODE;

    const int source =
        ::open(QFile::encodeName(package).constData(), O_RDONLY | O_CLOEXEC);
    if (source < 0) {
        return std::unexpected(QStringLiteral("cannot read %1: %2")
                                   .arg(package, ErrnoText(errno)));
    }
    struct SourceGuard {
        int Fd;
        ~SourceGuard() { ::close(Fd); }
    } sourceGuard{source};

    TemporaryFile temporary;
    QByteArray pattern = targetPath + ".update-XXXXXX";
    temporary.Fd = ::mkstemp(pattern.data());
    if (temporary.Fd < 0) {
        const int error = errno;
        const QString where = QFileInfo(target).absolutePath();
        const bool permission =
            error == EACCES || error == EPERM || error == EROFS;
        return std::unexpected(
            QStringLiteral("cannot create a file in %1: %2%3")
                .arg(where, ErrnoText(error),
                     permission ? ManualSteps() : QString()));
    }
    temporary.Path = pattern;
    const QString temporaryPath = QFile::decodeName(pattern);

    // The package is read once: what is installed is what was hashed.
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (auto copied = Copy(source, package, temporary.Fd, temporaryPath, hash);
        !copied) {
        return copied;
    }
    if (hash.result().toHex() != digest.toLower()) {
        return std::unexpected(
            QStringLiteral("%1: the package does not match its line in "
                           "SHA256SUMS")
                .arg(package));
    }
    if (::fchmod(temporary.Fd, mode) != 0) {
        return std::unexpected(QStringLiteral("cannot set the mode of %1: %2")
                                   .arg(temporaryPath, ErrnoText(errno)));
    }
    if (::fsync(temporary.Fd) != 0) {
        return std::unexpected(QStringLiteral("cannot sync %1: %2")
                                   .arg(temporaryPath, ErrnoText(errno)));
    }
    if (::close(temporary.Fd) != 0) {
        temporary.Fd = -1;
        return std::unexpected(QStringLiteral("cannot close %1: %2")
                                   .arg(temporaryPath, ErrnoText(errno)));
    }
    temporary.Fd = -1;
    if (::rename(pattern.constData(), targetPath.constData()) != 0) {
        const int error = errno;
        const bool permission = error == EACCES || error == EPERM ||
                                error == EROFS || error == EBUSY;
        return std::unexpected(
            QStringLiteral("cannot replace %1: %2%3")
                .arg(target, ErrnoText(error),
                     permission ? ManualSteps() : QString()));
    }
    temporary.Release();
    return {};
}

}  // namespace roost
