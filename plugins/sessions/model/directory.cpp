#include "sessions/model/directory.hpp"

#include <QDir>
#include <QFileInfo>

namespace roost::sessions {

namespace {

// `home` itself or a path under it, as a prefix of `path` that ends at a
// separator or at the end of the path.
bool IsUnder(const QString& path, const QString& home) {
    return path.startsWith(home) &&
           (path.size() == home.size() || path.at(home.size()) == u'/');
}

QString Expanded(const QString& path) {
    if (path == u'~' || path.startsWith(QStringLiteral("~/"))) {
        return QDir::homePath() + path.mid(1);
    }
    return path;
}

}  // namespace

DirectoryCheck CheckDirectory(const QString& typed) {
    if (typed.trimmed().isEmpty()) {
        return {typed.trimmed(), QStringLiteral("No directory")};
    }
    // The text as typed wins when it names an existing path, so that a
    // directory whose name ends with a blank (a session's cwd) is usable.
    QString path = Expanded(typed);
    if (!QFileInfo::exists(path)) {
        path = Expanded(typed.trimmed());
    }
    if (!QDir::isAbsolutePath(path)) {
        return {path, path + QStringLiteral(" is not an absolute path")};
    }
    const QFileInfo info(path);
    if (!info.exists()) {
        return {path, path + QStringLiteral(" does not exist")};
    }
    if (!info.isDir()) {
        return {path, path + QStringLiteral(" is not a directory")};
    }
    if (!info.isReadable()) {
        return {path, path + QStringLiteral(" cannot be read")};
    }
    if (!info.isExecutable()) {
        return {path, path + QStringLiteral(" cannot be entered")};
    }
    return {path, {}};
}

QString ShownDirectory(const QString& path) {
    const QString home = QDir::homePath();
    if (home.size() > 1 && IsUnder(path, home)) {
        return QStringLiteral("~") + path.mid(home.size());
    }
    return path;
}

}  // namespace roost::sessions
