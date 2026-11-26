#pragma once

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <map>
#include <optional>
#include <set>

#include "core/key_chord.hpp"
#include "core/rpc.hpp"
#include "roostd/server.hpp"

namespace roost {

class PluginRegistry;

// The keymap over the socket (design.md §16): the keybindings that plugins
// contribute, put together with the user's keymap.json in the config
// directory. It adds to the server:
//
//   keymap.get {platform}   {bindings, problems}
//     platform is "mac" or "linux", that of the window that asks: the daemon
//     may run on another machine. bindings are {key, command, args?, when?,
//     source}, key in its normal text for that platform, source "user" for the
//     file or the id of the plugin; problems are strings for the user. The
//     plugin of a key is chosen by PluginRegistry as for that platform, the
//     file is put over it: the pair (key, when) has the last entry of the file
//     that binds it, else the key of the first plugin; an entry with
//     "command": null removes the pair as it stands at that point, from
//     plugins and from the entries above. InvalidParams for another platform;
//     InternalError if the answer does not fit a frame.
//
//   keymap.setCommand {command, args?, when?, entries}   null
//     Writes the user's keys of one command to keymap.json: the entries of the
//     file that have this command, the same args (none if left out) and the
//     same when (none if left out) are replaced by `entries`, an array of
//     {key?, mac?} (at least one of them, as in the file), appended at the
//     end of the file. With `entries` null or empty the command is unbound
//     instead: its own entries go, and for each key that the command still
//     has (from the plugins) an entry with "command": null is appended. The
//     entries of other commands stay as they are, in their order; the file is
//     rewritten as indented JSON, so its formatting is not kept. A missing
//     file is created. InvalidParams for a command no plugin has, for a key
//     that does not parse, for an entry that the file would flag as a problem
//     (a key with no Cmd, Ctrl or Alt and no `when`), for params that are not
//     of this shape; StoreFailed, and the file untouched, if the file is not
//     JSON or not an array, is over 1 MiB (also after the change), cannot be
//     read or written.
//
//   keymap.resetCommand {command, args?, when?}   null
//     Takes the user's keys of one command out of the file: its entries as
//     above, and the "command": null entries that remove a key of its own
//     contribution (with no args), so the keys of the plugins apply again.
//     The same errors as keymap.setCommand, but a command that no plugin has
//     is no error: its entries are taken out all the same.
//
// and publishes on the bus, for each platform that a connected window asked
// for with keymap.get, when keymap.json has changed:
//
//   keymap.changed {platform, bindings, problems}
//
// with what keymap.get would answer. A table that does not fit a frame is not
// published; qWarning says so.
//
// A file that is not JSON, not an array, unreadable or over 1 MiB leaves the
// entries of the version before it and adds a problem; at start there is no
// version before it, and the plugins' keys stand alone. A broken entry is
// skipped with a problem. A missing file is no problem. roostd writes the file
// only on keymap.setCommand and keymap.resetCommand, and does not publish
// them: the watcher below sees the write as it sees a hand edit.
//
// The file is watched as PluginWatcher watches a plugin directory (the recipe
// is the same: the file and its directory, an event counts when the
// modification time, the size or the change time differ from the last seen,
// 100 ms after the last event the file is read). The directory is also the
// place where other documents of the store are written, so a change that
// leaves the file as it was is not published.
//
// The plugin registry is read at each request. Nothing publishes the table
// when the plugins change: the host loads them once, before this service, and
// has no event for a change.
//
// The server and the registry must outlive it; after it is gone, its method
// fails with InternalError.
class KeymapService : public QObject {
    Q_OBJECT

public:
    static constexpr qint64 MAX_FILE_BYTES = 1024 * 1024;

    KeymapService(QString configDirectory, Server& server,
                  const PluginRegistry& plugins, QObject* parent = nullptr);
    ~KeymapService() override;

    KeymapService(const KeymapService&) = delete;
    KeymapService& operator=(const KeymapService&) = delete;

private:
    struct Table {
        QJsonArray Bindings;
        QJsonArray Problems;
    };
    // What an event of a path is compared with.
    struct Signature {
        bool Exists = false;
        qint64 Size = 0;
        QDateTime Modified;
        QDateTime Changed;
        bool operator==(const Signature&) const = default;
    };
    static Signature SignatureOf(const QString& path);

    Table Resolve(EPlatform platform) const;
    // The table of the plugins with `entries` over them; `problem` is the one
    // of the file.
    Table ResolveWith(EPlatform platform,
                      const std::optional<QJsonArray>& entries,
                      const QString& problem) const;
    // The keymap.setCommand and keymap.resetCommand methods.
    rpc::Result SetCommand(const QJsonObject& params);
    rpc::Result ResetCommand(const QJsonObject& params);
    // Reads the file; keeps the entries of before if it cannot be used.
    void ReadFile();
    // Brings the watched paths to the file, if it exists, and the directory,
    // or the nearest one that exists.
    void Sync();
    void OnPathChanged(const QString& path);
    void OnQuiet();
    void PublishAll();

    QString Directory_;
    QString Path_;
    Server& Server_;
    const PluginRegistry& Plugins_;

    // The entries of the last file that could be used; none if there is no
    // such file.
    std::optional<QJsonArray> Entries_;
    // Why the file as it is now is not used; empty if it is.
    QString Problem_;

    // The platforms that each open connection asked for.
    std::map<Server::ConnectionId, std::set<EPlatform>> Asked_;

    QFileSystemWatcher Watcher_;
    QTimer Quiet_;
    QHash<QString, Signature> Seen_;
};

}  // namespace roost
