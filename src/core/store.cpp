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

bool IsIdStart(QChar c) {
    return (c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9');
}

}  // namespace

QString ConfigDirectory() {
    const QString roost = qEnvironmentVariable("ROOST_CONFIG");
    if (!roost.isEmpty()) {
        return QDir(roost).absolutePath();
    }
    // The XDG spec says a relative value is invalid and must be ignored.
    const QString xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (QDir::isAbsolutePath(xdg)) {
        return QDir::cleanPath(xdg + QStringLiteral("/roost"));
    }
    return QDir::home().filePath(QStringLiteral(".config/roost"));
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

QString Store::PathOf(const QString& pluginId) const {
    return Directory_ + u'/' + pluginId + QStringLiteral(".json");
}

std::expected<QJsonObject, QString> Store::Load(const QString& pluginId) const {
    if (!IsValidPluginId(pluginId)) {
        return std::unexpected(
            QStringLiteral("invalid plugin id \"%1\"").arg(pluginId));
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
    if (!IsValidPluginId(pluginId)) {
        return std::unexpected(
            QStringLiteral("invalid plugin id \"%1\"").arg(pluginId));
    }
    if (!QDir(Directory_).exists()) {
        if (!QDir().mkpath(Directory_)) {
            return std::unexpected(
                QStringLiteral("cannot create %1").arg(Directory_));
        }
        // Plugin state may hold tokens: only the owner gets in.
        QFile::setPermissions(
            Directory_, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    const QString path = PathOf(pluginId);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return std::unexpected(QStringLiteral("cannot write %1: %2")
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
