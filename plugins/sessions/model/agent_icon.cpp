#include "sessions/model/agent_icon.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace roost::sessions {

namespace {

// The module RoostSessions keeps the bundled icons in its resources.
const QString BUNDLED_DIRECTORY =
    QStringLiteral(":/qt/qml/RoostSessions/agent-icons/");
const QString BUNDLED_URL =
    QStringLiteral("qrc:/qt/qml/RoostSessions/agent-icons/");

QUrl Bundled(const QString& name) { return QUrl(BUNDLED_URL + name + ".svg"); }

}  // namespace

QString UserAgentIconsDirectory() {
    QString config;
    const QString roost = qEnvironmentVariable("ROOST_CONFIG");
    const QString xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (!roost.isEmpty()) {
        config = QDir::cleanPath(QDir(roost).absolutePath());
    } else if (QDir::isAbsolutePath(xdg)) {
        config = QDir::cleanPath(xdg + QStringLiteral("/roost"));
    } else {
        const QString home = qEnvironmentVariable("HOME");
        if (!QDir::isAbsolutePath(home) || QDir::cleanPath(home) == u'/') {
            return {};
        }
        config = QDir::cleanPath(home + QStringLiteral("/.config/roost"));
    }
    return config + QStringLiteral("/agent-icons");
}

bool IsAgentIconName(const QString& agent) {
    static const QRegularExpression PATTERN(
        QStringLiteral("\\A[A-Za-z0-9._-]+\\z"));
    return PATTERN.match(agent).hasMatch() && agent != QStringLiteral(".") &&
           agent != QStringLiteral("..");
}

namespace {

// The user's `<name>.svg`, then `.png`; empty when there is none.
QUrl UserIcon(const QString& directory, const QString& name) {
    if (directory.isEmpty()) {
        return {};
    }
    for (const QString& extension :
         {QStringLiteral(".svg"), QStringLiteral(".png")}) {
        const QFileInfo file(directory + '/' + name + extension);
        if (file.isFile()) {
            return QUrl::fromLocalFile(file.absoluteFilePath());
        }
    }
    return {};
}

}  // namespace

QUrl ResolveBundledAgentIcon(const QString& agent) {
    if (IsAgentIconName(agent) &&
        QFile::exists(BUNDLED_DIRECTORY + agent + ".svg")) {
        return Bundled(agent);
    }
    return Bundled(QStringLiteral("default"));
}

QUrl ResolveAgentIcon(const QString& agent) {
    const QString directory = UserAgentIconsDirectory();
    const bool plain = IsAgentIconName(agent);
    if (plain) {
        if (const QUrl own = UserIcon(directory, agent); !own.isEmpty()) {
            return own;
        }
        if (QFile::exists(BUNDLED_DIRECTORY + agent + ".svg")) {
            return Bundled(agent);
        }
    }
    if (const QUrl fallback = UserIcon(directory, QStringLiteral("default"));
        !fallback.isEmpty()) {
        return fallback;
    }
    return Bundled(QStringLiteral("default"));
}

}  // namespace roost::sessions
