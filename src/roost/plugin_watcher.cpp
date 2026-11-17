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

// The directory as the watcher compares paths with it: clean and absolute.
// Empty for a directory that cannot be watched (empty, relative, the root):
// its prefix would match every path.
QString CleanDirectory(const QString& directory) {
    if (!QDir::isAbsolutePath(directory)) {
        return {};
    }
    const QString clean = QDir::cleanPath(directory);
    return clean == u"/" ? QString() : clean;
}

bool Within(const QString& path, const QString& directory) {
    return path == directory || path.startsWith(directory + u'/');
}

QString ParentOf(const QString& directory) {
    return QDir::cleanPath(directory + u"/..");
}

// The directories and the `*.qml`, `*.js` and `qmldir` files under
// `directory` (itself included among the directories), sorted.
void Collect(const QString& directory, QStringList& directories,
             QStringList& files) {
    if (!QFileInfo(directory).isDir()) {
        return;
    }
    directories.append(directory);
    QDirIterator it(directory, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    QStringList foundDirectories;
    QStringList foundFiles;
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        if (info.isDir()) {
            foundDirectories.append(it.filePath());
        } else if (info.suffix() == u"qml" || info.suffix() == u"js" ||
                   info.fileName() == u"qmldir") {
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
    QStringList parents;
    QSet<QString> pluginDirectories;
    for (const QString& declared : plugins) {
        const QString directory = CleanDirectory(declared);
        if (directory.isEmpty()) {
            continue;
        }
        pluginDirectories.insert(directory);
        Present_.insert(directory, QFileInfo(directory).isDir());
        Collect(directory, directories, files);
        // The parent shows a plugin directory that is deleted and made again.
        const QString parent = ParentOf(directory);
        if (QFileInfo(parent).isDir()) {
            parents.append(parent);
        }
    }
    // Directories first: they catch new and replaced files.
    QStringList all = directories + parents + files;
    QStringList wanted;
    QSet<QString> wantedSet;
    for (const QString& path : all) {
        if (!wantedSet.contains(path)) {
            wantedSet.insert(path);
            wanted.append(path);
        }
    }
    QStringList unwatched;
    if (wanted.size() > MaxPaths_) {
        unwatched = wanted.mid(MaxPaths_);
        wanted = wanted.first(MaxPaths_);
        for (const QString& path : unwatched) {
            wantedSet.remove(path);
        }
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

    for (auto it = Present_.begin(); it != Present_.end();) {
        it = pluginDirectories.contains(it.key()) ? std::next(it)
                                                  : Present_.erase(it);
    }
    for (auto it = Timers_.begin(); it != Timers_.end();) {
        if (plugins.contains(it.key())) {
            ++it;
        } else {
            it.value()->stop();
            it.value()->deleteLater();
            it = Timers_.erase(it);
        }
    }

    // What Qt watches now is what it has kept: a file replaced by rename is
    // gone from it.
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
    if (missing.isEmpty()) {
        return;
    }
    const QStringList failed = Watcher_.addPaths(missing);
    if (!failed.isEmpty()) {
        qWarning("roost --dev: cannot watch: %s",
                 qPrintable(failed.join(u", ")));
    }
    // A change between the signature and addPaths has no event: look again.
    for (const QString& path : missing) {
        const auto seen = Seen_.find(path);
        if (failed.contains(path) || seen == Seen_.end()) {
            continue;
        }
        const Signature now = SignatureOf(path);
        if (seen.value() != now) {
            seen.value() = now;
            for (const QString& id : AffectedBy(path, plugins)) {
                StartTimer(id);
            }
        }
    }
}

QStringList PluginWatcher::AffectedBy(const QString& path,
                                      const QMap<QString, QString>& plugins) {
    QStringList ids;
    for (auto it = plugins.begin(); it != plugins.end(); ++it) {
        const QString directory = CleanDirectory(it.value());
        if (directory.isEmpty()) {
            continue;
        }
        if (Within(path, directory)) {
            if (path == directory) {
                Present_.insert(directory, QFileInfo(directory).isDir());
            }
            ids.append(it.key());
        } else if (path == ParentOf(directory)) {
            // A plugin directory that appeared or went away.
            const bool present = QFileInfo(directory).isDir();
            const auto known = Present_.find(directory);
            if (known == Present_.end() || known.value() != present) {
                Present_.insert(directory, present);
                ids.append(it.key());
            }
        }
    }
    return ids;
}

void PluginWatcher::StartTimer(const QString& pluginId) {
    QTimer*& timer = Timers_[pluginId];
    if (timer == nullptr) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(Debounce_);
        connect(timer, &QTimer::timeout, this,
                [this, pluginId] { OnQuiet(pluginId); });
    }
    timer->start();
}

void PluginWatcher::OnPathChanged(const QString& path) {
    const Signature now = SignatureOf(path);
    const auto seen = Seen_.find(path);
    if (seen != Seen_.end()) {
        if (seen.value() == now) {
            // Qt may have dropped the path (inotify does on rename and
            // delete) with nothing else to show for it: bring it back.
            if (!Watcher_.files().contains(path) &&
                !Watcher_.directories().contains(path)) {
                Sync();
            }
            return;
        }
        seen.value() = now;
    }
    for (const QString& id : AffectedBy(path, Loader_.PluginDirectories())) {
        StartTimer(id);
    }
}

void PluginWatcher::OnQuiet(const QString& pluginId) {
    // Files that appeared since the last event, a directory that came back.
    Sync();
    const QString directory =
        CleanDirectory(Loader_.PluginDirectories().value(pluginId));
    if (directory.isEmpty()) {
        return;
    }
    // The reload below reads what Sync may have found changed.
    if (QTimer* timer = Timers_.value(pluginId)) {
        timer->stop();
    }
    // What was written since the last event of a path is taken as seen: the
    // access time that the reload changes must not look like an edit, and a
    // write whose event was merged with an earlier one has been read.
    for (auto it = Seen_.begin(); it != Seen_.end(); ++it) {
        if (Within(it.key(), directory)) {
            it.value() = SignatureOf(it.key());
        }
    }
    Loader_.Reload(pluginId);
    emit Reloaded(pluginId);
}

}  // namespace roost
