#pragma once

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>

#include "roost/plugin_loader.hpp"

namespace roost {

// The developer mode (`roost --dev`): reloads the QML of a plugin when its
// files change. Plain C++, not a QML type; GUI thread only.
//
// For each loaded plugin it watches every `*.qml`, `*.js` and `qmldir` file
// under the plugin directory (recursive, symlinks to directories are not
// followed), the directories themselves and the parent of the plugin
// directory (not recursive; shared by plugins, counted once). A plugin whose
// directory is empty or not absolute is skipped. The recipe is that of the
// keymap (design.md §16): a file that an editor replaces by rename drops out of
// the QFileSystemWatcher, and the event of its directory brings it back,
// because the plugin is re-scanned before each reload. A file event or a
// directory event marks the plugin dirty; Debounce after the last event for it,
// the watcher calls PluginLoader::Reload(id) from a timer (never from inside a
// JS call) and emits Reloaded(id). Plugins are independent: one timer for each.
//
// A deleted file is one reload (the panel that needs it becomes a problem of
// the loader); the file written again is another. A deleted plugin directory
// is one reload too (all its panels become problems); when it is made again,
// the event of its parent starts the reload that brings the plugin back.
//
// The set of watched paths follows the loader (PluginsChanged). The total is
// capped at MaxPaths (each path costs a file descriptor on macOS): directories
// (parents included) go first, then files, plugin by plugin in load order; what
// does not fit is not watched, and a qWarning names it. The warning comes when
// the unwatched set differs from the one last warned about, so a re-scan on
// every event does not repeat it.
//
// Reading a file or a directory (the reload itself does) changes its access
// time, and the watcher then reports it. An event counts only if the
// modification time, the size or the change time of the path differ from what
// the watcher last saw; the access time is not among them.
//
// Manifests and native libraries are not watched: a manifest is read by
// roostd at its start, and a changed one reaches the loader only through a
// new plugins.list.
//
// It must be destroyed before the loader.
class PluginWatcher : public QObject {
    Q_OBJECT

public:
    PluginWatcher(
        PluginLoader& loader,
        std::chrono::milliseconds debounce = std::chrono::milliseconds(100),
        int maxPaths = 256, QObject* parent = nullptr);

signals:
    // After Reload of the plugin.
    void Reloaded(const QString& pluginId);

private:
    // What an event of a path is compared with.
    struct Signature {
        bool Exists = false;
        qint64 Size = 0;
        QDateTime Modified;
        QDateTime Changed;
        bool operator==(const Signature&) const = default;
    };
    static Signature SignatureOf(const QString& path);

    // Brings the watched set to what the plugins have now.
    void Sync();
    // The plugins that an event of `path` concerns: the path is under the
    // directory of the plugin, or is its parent and the directory appeared or
    // went away since the last look.
    QStringList AffectedBy(const QString& path,
                           const QMap<QString, QString>& plugins);
    void StartTimer(const QString& pluginId);
    void OnPathChanged(const QString& path);
    void OnQuiet(const QString& pluginId);

    PluginLoader& Loader_;
    std::chrono::milliseconds Debounce_;
    int MaxPaths_;
    QFileSystemWatcher Watcher_;
    // Plugin id -> its timer, started by each event.
    QHash<QString, QTimer*> Timers_;
    // Plugin directory -> whether it existed at the last look.
    QHash<QString, bool> Present_;
    // The signature of each watched path as of its last event, or as of when
    // it began to be watched.
    QHash<QString, Signature> Seen_;
    // What the last warning named.
    QSet<QString> Unwatched_;
};

}  // namespace roost
