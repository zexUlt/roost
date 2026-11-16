#include "core/store.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

#include <utility>

namespace roost {

namespace {

constexpr qsizetype MAX_PLUGIN_ID_LENGTH = 64;
const QString RESERVED_PLUGIN_ID_KEYMAP = QStringLiteral("keymap");

bool IsIdStart(QChar c) {
    return (c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9');
}

}  // namespace

std::expected<QString, QString> ConfigDirectory() {
    const QString roost = qEnvironmentVariable("ROOST_CONFIG");
    if (!roost.isEmpty()) {
        return QDir::cleanPath(QDir(roost).absolutePath());
    }
    // The XDG spec says a relative value is invalid and must be ignored.
    const QString xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (QDir::isAbsolutePath(xdg)) {
        return QDir::cleanPath(xdg + QStringLiteral("/roost"));
    }
    // Without HOME, Qt falls back to the root directory, and a daemon would
    // scatter its state there.
    const QString home = qEnvironmentVariable("HOME");
    if (!QDir::isAbsolutePath(home) || QDir::cleanPath(home) == u'/') {
        return std::unexpected(QStringLiteral(
            "HOME is not set to a home directory; set ROOST_CONFIG"));
    }
    return QDir::cleanPath(home + QStringLiteral("/.config/roost"));
}

bool Store::IsReservedPluginId(const QString& id) {
    return id == RESERVED_PLUGIN_ID_KEYMAP;
}

Store::Store(QString directory) : Directory_(std::move(directory)) {}

bool Store::IsValidPluginId(const QString& id) {
    if (id.isEmpty() || id.size() > MAX_PLUGIN_ID_LENGTH ||
        !IsIdStart(id.front())) {
        return false;
    }
    for (const QChar c : id) {
        if (!IsIdStart(c) && c != u'.' && c != u'_' && c != u'-') {
            return false;
        }
    }
    return true;
}

std::expected<void, QString> Store::CheckRequest(
    const QString& pluginId) const {
    if (!QDir::isAbsolutePath(Directory_)) {
        return std::unexpected(
            QStringLiteral("store directory \"%1\" is not an absolute path")
                .arg(Directory_));
    }
    if (!IsValidPluginId(pluginId)) {
        return std::unexpected(
            QStringLiteral("invalid plugin id \"%1\"").arg(pluginId));
    }
    if (IsReservedPluginId(pluginId)) {
        return std::unexpected(
            QStringLiteral("plugin id \"%1\" is reserved").arg(pluginId));
    }
    return {};
}

QString Store::PathOf(const QString& pluginId) const {
    return Directory_ + u'/' + pluginId + QStringLiteral(".json");
}

std::expected<QJsonObject, QString> Store::Load(const QString& pluginId) const {
    if (auto checked = CheckRequest(pluginId); !checked) {
        return std::unexpected(checked.error());
    }
    const QString path = PathOf(pluginId);
    QFile file(path);
    if (!file.exists()) {
        return QJsonObject();
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(
            QStringLiteral("cannot read %1: %2").arg(path, file.errorString()));
    }
    QJsonParseError error{};
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError) {
        return std::unexpected(QStringLiteral("%1 is not valid JSON: %2 at "
                                              "offset %3")
                                   .arg(path, error.errorString())
                                   .arg(error.offset));
    }
    if (!document.isObject()) {
        return std::unexpected(
            QStringLiteral("%1 does not hold a JSON object").arg(path));
    }
    return document.object();
}

std::expected<void, QString> Store::Save(const QString& pluginId,
                                         const QJsonObject& document) const {
    if (auto checked = CheckRequest(pluginId); !checked) {
        return checked;
    }
    if (!QDir(Directory_).exists()) {
        if (!QDir().mkpath(Directory_)) {
            return std::unexpected(
                QStringLiteral("cannot create %1").arg(Directory_));
        }
        // Plugin state may hold tokens: only the owner gets in.
        if (!QFile::setPermissions(Directory_, QFile::ReadOwner |
                                                   QFile::WriteOwner |
                                                   QFile::ExeOwner)) {
            return std::unexpected(
                QStringLiteral("cannot restrict %1 to its owner")
                    .arg(Directory_));
        }
    }

    const QString path = PathOf(pluginId);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return std::unexpected(QStringLiteral("cannot write %1: %2")
                                   .arg(path, file.errorString()));
    }
    // The file is owner-only even in a directory someone made more open, and
    // even when an earlier version of it was not: QSaveFile copies the old
    // file's mode to the temporary one on open, so this comes after.
    if (!file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
        return std::unexpected(QStringLiteral("cannot restrict %1: %2")
                                   .arg(path, file.errorString()));
    }
    file.write(QJsonDocument(document).toJson(QJsonDocument::Indented));
    // A failed write above makes the commit fail and drops the temporary
    // file; the old document stays as it was.
    if (!file.commit()) {
        return std::unexpected(QStringLiteral("cannot write %1: %2")
                                   .arg(path, file.errorString()));
    }
    return {};
}

}  // namespace roost
