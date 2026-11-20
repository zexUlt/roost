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
        config = QDir::cleanPath(qEnvironmentVariable("HOME") +
                                 QStringLiteral("/.config/roost"));
    }
    return config + QStringLiteral("/agent-icons");
}

bool IsAgentIconName(const QString& agent) {
    static const QRegularExpression plain(
        QStringLiteral("\\A[A-Za-z0-9._-]+\\z"));
    return plain.match(agent).hasMatch() && agent != QStringLiteral(".") &&
           agent != QStringLiteral("..");
}

QUrl ResolveAgentIcon(const QString& agent) {
    if (!IsAgentIconName(agent)) {
        return Bundled(QStringLiteral("default"));
    }
    const QString directory = UserAgentIconsDirectory();
    for (const QString& extension :
         {QStringLiteral(".svg"), QStringLiteral(".png")}) {
        const QFileInfo file(directory + '/' + agent + extension);
        if (file.isFile()) {
            return QUrl::fromLocalFile(file.absoluteFilePath());
        }
    }
    if (QFile::exists(BUNDLED_DIRECTORY + agent + ".svg")) {
        return Bundled(agent);
    }
    return Bundled(QStringLiteral("default"));
}

}  // namespace roost::sessions
