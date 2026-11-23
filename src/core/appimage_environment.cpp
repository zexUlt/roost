#include "core/appimage_environment.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <sys/stat.h>

namespace roost {

namespace {
const QString EXTRACT_AND_RUN = QStringLiteral("APPIMAGE_EXTRACT_AND_RUN");
}  // namespace

const QStringList& AppImageBundleVariables() {
    static const QStringList VARIABLES{
        QStringLiteral("APPDIR"), QStringLiteral("APPIMAGE"),
        QStringLiteral("ARGV0"), QStringLiteral("OWD")};
    return VARIABLES;
}

const QStringList& AppImageLibraryVariables() {
    static const QStringList VARIABLES{QStringLiteral("LD_LIBRARY_PATH"),
                                       QStringLiteral("QT_PLUGIN_PATH"),
                                       QStringLiteral("QML2_IMPORT_PATH")};
    return VARIABLES;
}

bool InsideAppImage(const QString& appDir, const QString& executableDir) {
    if (appDir.isEmpty()) {
        return false;
    }
    const QString root = QDir::cleanPath(appDir);
    const QString executable = QDir::cleanPath(executableDir);
    return executable == root || executable.startsWith(root + QLatin1Char('/'));
}

QProcessEnvironment StripAppImageEnvironment(QProcessEnvironment environment,
                                             bool inAppImage) {
    for (const QString& name : AppImageBundleVariables()) {
        environment.remove(name);
    }
    if (inAppImage) {
        for (const QString& name : AppImageLibraryVariables()) {
            environment.remove(name);
        }
    }
    return environment;
}

std::optional<bool> IsMountPoint(const QString& directory) {
    if (directory.isEmpty()) {
        return std::nullopt;
    }
    struct stat self{};
    struct stat parent{};
    const QByteArray path = QFile::encodeName(directory);
    const QByteArray up = QFile::encodeName(directory + QStringLiteral("/.."));
    if (::stat(path.constData(), &self) != 0 ||
        ::stat(up.constData(), &parent) != 0) {
        return std::nullopt;
    }
    return self.st_dev != parent.st_dev;
}

QProcessEnvironment RelaunchEnvironment(const QProcessEnvironment& environment,
                                        bool appDirIsMountPoint) {
    const bool extracted =
        !appDirIsMountPoint &&
        !environment.value(QStringLiteral("APPDIR")).isEmpty();
    QProcessEnvironment result = StripAppImageEnvironment(environment, true);
    if (extracted && !result.contains(EXTRACT_AND_RUN)) {
        result.insert(EXTRACT_AND_RUN, QStringLiteral("1"));
    }
    return result;
}

QString RelaunchDirectory(const QProcessEnvironment& environment,
                          const QString& home) {
    const QString owd = environment.value(QStringLiteral("OWD"));
    return !owd.isEmpty() && QFileInfo(owd).isDir() ? owd : home;
}

}  // namespace roost
