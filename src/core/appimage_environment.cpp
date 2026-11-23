#include "core/appimage_environment.hpp"

#include <QDir>
#include <QFileInfo>

namespace roost {

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

QProcessEnvironment RelaunchEnvironment(
    const QProcessEnvironment& environment) {
    return StripAppImageEnvironment(environment, true);
}

QString RelaunchDirectory(const QProcessEnvironment& environment,
                          const QString& home) {
    const QString owd = environment.value(QStringLiteral("OWD"));
    return !owd.isEmpty() && QFileInfo(owd).isDir() ? owd : home;
}

}  // namespace roost
