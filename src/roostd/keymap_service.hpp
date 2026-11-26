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
//     Sets the whole set of the keys of one command, in one write of
//     keymap.json. `entries` is an array of {key?, mac?} (at least one of
//     them, as in the file, and no other field: command, args and when are
//     those of the request): the keys that the command must have afterwards
//     for this args (none if left out) and this when (none if left out), on
//     each platform. The entries of the file that have this command, args and
//     when are replaced by those of `entries`, appended at the end of the
//     file; a key that the plugins give the command already, and `entries`
//     has, is not written again (unless another entry of the file holds it).
//     Each key that the plugins give the command for this `when` (they give
//     no args) and `entries` does not have gets an entry with "command": null
//     at the start of the file, so it removes the plugins' key and nothing
//     that the file binds below, even when another entry holds the key now.
//     Each such entry takes the keys of this command only, on each platform:
//     the Mac's and Linux's keys go in pairs {key, mac}, and what has no pair
//     has an entry of its own (see keymap_service.cpp). `entries` empty
//     removes every key of the command; to add a key, send the keys the
//     command has now and the new one. The entries "command": null that an
//     earlier call wrote for this command are taken out and made again, the
//     rest of the file stays as it is, in its order; the file is rewritten as
//     indented JSON, so its formatting is not kept. A missing file is
//     created, in a directory made 0700 if it is missing, the file 0600 (as
//     the store's own). All or nothing: one entry that is refused refuses the
//     call, and the file is not touched. InvalidParams for a command no
//     plugin has, for a key that does not parse, for an entry with a field
//     other than key and mac or that the file would flag as a problem (a key
//     with no Cmd, Ctrl or Alt and no `when`), for `entries` that is not an
//     array (null too), for params that are not of this shape; StoreFailed,
//     and the file untouched, if the file is not JSON or not an array, is
//     over 1 MiB (also after the change), cannot be read or written. The
//     handler runs to its end in the event loop: calls of two clients do not
//     interleave, and the second reads the file the first wrote.
//
//   keymap.resetCommand {command, args?, when?}   null
//     Returns the command to what the plugins give it: takes out of the file
//     what setCommand writes for this args and when: the entries of the
//     command, and the entries "command": null whose only effect, on each
//     platform where they apply, is to remove keys of the command with this
//     args and when (and which remove one on some platform; those written by
//     hand for another command's key, or for no key, stay). The same errors
//     as keymap.setCommand, but a command that no plugin has is no error: its
//     entries are taken out all the same.
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
