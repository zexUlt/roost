#include "roost/plugin_watcher.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QtLogging>

#include <algorithm>
#include <iterator>

namespace roost {

namespace {

constexpr int NAMED_UNWATCHED = 10;

// The directories and the `*.qml` and `*.js` files under `directory`
// (itself included among the directories), sorted.
void Collect(const QString& directory, QStringList& directories,
             QStringList& files) {
    if (!QFileInfo(directory).isDir()) {
        return;
    }
    directories.append(QDir::cleanPath(directory));
    QDirIterator it(directory, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    QStringList foundDirectories;
    QStringList foundFiles;
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        if (info.isDir()) {
            foundDirectories.append(it.filePath());
        } else if (info.suffix() == u"qml" || info.suffix() == u"js") {
            foundFiles.append(it.filePath());
        }
    }
    foundDirectories.sort();
    foundFiles.sort();
    directories += foundDirectories;
    files += foundFiles;
}

}  // namespace

PluginWatcher::PluginWatcher(PluginLoader& loader,
                             std::chrono::milliseconds debounce, int maxPaths,
                             QObject* parent)
    : QObject(parent),
      Loader_(loader),
      Debounce_(debounce),
      MaxPaths_(maxPaths),
      Watcher_(this) {
    connect(&Watcher_, &QFileSystemWatcher::fileChanged, this,
            &PluginWatcher::OnPathChanged);
    connect(&Watcher_, &QFileSystemWatcher::directoryChanged, this,
            &PluginWatcher::OnPathChanged);
    connect(&Loader_, &PluginLoader::PluginsChanged, this,
            &PluginWatcher::Sync);
    Sync();
}

PluginWatcher::Signature PluginWatcher::SignatureOf(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists()) {
        return {};
    }
    return {true, info.size(), info.lastModified(), info.metadataChangeTime()};
}

void PluginWatcher::Sync() {
    const QMap<QString, QString> plugins = Loader_.PluginDirectories();

    QStringList directories;
    QStringList files;
    for (const QString& directory : plugins) {
        Collect(directory, directories, files);
    }
    // Directories first: they catch new and replaced files.
    QStringList wanted = directories + files;
    QStringList unwatched;
    if (wanted.size() > MaxPaths_) {
        unwatched = wanted.mid(MaxPaths_);
        wanted = wanted.first(MaxPaths_);
    }
    const QSet<QString> unwatchedSet(unwatched.begin(), unwatched.end());
    if (!unwatchedSet.isEmpty() && unwatchedSet != Unwatched_) {
        QStringList named = unwatched.first(
            std::min<qsizetype>(unwatched.size(), NAMED_UNWATCHED));
        qWarning(
            "roost --dev: %lld of %lld paths do not fit the limit of %d "
            "and are not watched: %s%s",
            static_cast<long long>(unwatched.size()),
            static_cast<long long>(unwatched.size() + wanted.size()), MaxPaths_,
            qPrintable(named.join(u", ")),
            unwatched.size() > named.size() ? ", ..." : "");
    }
    Unwatched_ = unwatchedSet;

    // What Qt watches now is what it has kept: a file replaced by rename is
    // gone from it.
    const QSet<QString> wantedSet(wanted.begin(), wanted.end());
    QStringList stale;
    QSet<QString> have;
    for (const QStringList& list : {Watcher_.files(), Watcher_.directories()}) {
        for (const QString& path : list) {
            have.insert(path);
            if (!wantedSet.contains(path)) {
                stale.append(path);
            }
        }
    }
    if (!stale.isEmpty()) {
        Watcher_.removePaths(stale);
    }
    QStringList missing;
    for (const QString& path : wanted) {
        if (!have.contains(path)) {
            missing.append(path);
        }
    }
    for (const QString& path : missing) {
        // A path already seen keeps its signature: its own event is yet to
        // come, and it must find the change.
        if (!Seen_.contains(path)) {
            Seen_.insert(path, SignatureOf(path));
        }
    }
    for (auto it = Seen_.begin(); it != Seen_.end();) {
        it = wantedSet.contains(it.key()) ? std::next(it) : Seen_.erase(it);
    }
    if (!missing.isEmpty()) {
        const QStringList failed = Watcher_.addPaths(missing);
        if (!failed.isEmpty()) {
            qWarning("roost --dev: cannot watch: %s",
                     qPrintable(failed.join(u", ")));
        }
    }

    for (auto it = Timers_.begin(); it != Timers_.end();) {
        if (plugins.contains(it.key())) {
            ++it;
        } else {
            it.value()->deleteLater();
            it = Timers_.erase(it);
        }
    }
}

void PluginWatcher::OnPathChanged(const QString& path) {
    const Signature now = SignatureOf(path);
    const auto seen = Seen_.find(path);
    if (seen != Seen_.end()) {
        if (seen.value() == now) {
            return;
        }
        seen.value() = now;
    }
    const QMap<QString, QString> plugins = Loader_.PluginDirectories();
    // Before the timers: a file that was replaced is watched again for the
    // next edit.
    Sync();
    for (auto it = plugins.begin(); it != plugins.end(); ++it) {
        const QString directory = QDir::cleanPath(it.value());
        if (path != directory && !path.startsWith(directory + u'/')) {
            continue;
        }
        QTimer*& timer = Timers_[it.key()];
        if (timer == nullptr) {
            timer = new QTimer(this);
            timer->setSingleShot(true);
            timer->setInterval(Debounce_);
            connect(timer, &QTimer::timeout, this,
                    [this, id = it.key()] { OnQuiet(id); });
        }
        timer->start();
    }
}

void PluginWatcher::OnQuiet(const QString& pluginId) {
    // A file that appeared since the last event.
    Sync();
    if (!Loader_.PluginDirectories().contains(pluginId)) {
        return;
    }
    // What was written since the last event of a path is taken as seen: the
    // access time that the reload changes must not look like an edit, and a
    // write whose event was merged with an earlier one has been read.
    const QString directory =
        QDir::cleanPath(Loader_.PluginDirectories().value(pluginId));
    for (auto it = Seen_.begin(); it != Seen_.end(); ++it) {
        if (it.key() == directory || it.key().startsWith(directory + u'/')) {
            it.value() = SignatureOf(it.key());
        }
    }
    Loader_.Reload(pluginId);
    emit Reloaded(pluginId);
}

}  // namespace roost
