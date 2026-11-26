#include "roostd/keymap_service.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>
#include <QtLogging>

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <set>
#include <variant>
#include <vector>

#include "core/protocol.hpp"
#include "core/rpc.hpp"
#include "core/store.hpp"
#include "roostd/manifest.hpp"

namespace roost {

namespace {

// After the last event, as in PluginWatcher.
constexpr std::chrono::milliseconds DEBOUNCE(100);

const QString CHANGED = QStringLiteral("keymap.changed");
const QString USER = QStringLiteral("user");

rpc::Result Fail(rpc::EErrorCode code, const QString& message) {
    return std::unexpected(rpc::Error::FromCode(code, message));
}

QString PlatformName(EPlatform platform) {
    return platform == EPlatform::Mac ? QStringLiteral("mac")
                                      : QStringLiteral("linux");
}

std::optional<EPlatform> PlatformOf(const QJsonValue& name) {
    if (name == PlatformName(EPlatform::Mac)) {
        return EPlatform::Mac;
    }
    if (name == PlatformName(EPlatform::Linux)) {
        return EPlatform::Linux;
    }
    return std::nullopt;
}

// The pair that a key is in the table: the chord in its normal text and the
// panel type, which cannot hold a NUL.
QString KeyOf(const KeyChord& chord, const QString& when) {
    return WriteKeyChord(chord) + QChar(u'\0') + when;
}

// One binding of the table.
struct Binding {
    KeyChord Chord;
    QString When;
    QString Command;
    QJsonValue Args;
    QString Source;
};

// An entry of the file, checked. No Command: it removes the pair.
struct Entry {
    KeyChord Chord;
    QString When;
    std::optional<QString> Command;
    QJsonValue Args;
};
// An entry only for the other platform: it does nothing here, and is no
// problem.
struct OtherPlatform {};

// A string field that may be left out; the message says what is wrong.
std::expected<std::optional<QString>, QString> OptionalString(
    const QJsonObject& object, const QString& name) {
    const QJsonValue value = object.value(name);
    if (value.isUndefined()) {
        return std::optional<QString>();
    }
    if (!value.isString()) {
        return std::unexpected(
            QStringLiteral("\"%1\" is not a string").arg(name));
    }
    return std::optional<QString>(value.toString());
}

// The error is for the user.
std::expected<std::variant<Entry, OtherPlatform>, QString> ParseEntry(
    const QJsonValue& value, EPlatform platform,
    const PluginRegistry& plugins) {
    if (!value.isObject()) {
        return std::unexpected(QStringLiteral("it is not an object"));
    }
    const QJsonObject object = value.toObject();
    const auto key = OptionalString(object, QStringLiteral("key"));
    if (!key) {
        return std::unexpected(key.error());
    }
    const auto mac = OptionalString(object, QStringLiteral("mac"));
    if (!mac) {
        return std::unexpected(mac.error());
    }
    if (!*key && !*mac) {
        return std::unexpected(
            QStringLiteral("it has neither \"key\" nor "
                           "\"mac\""));
    }
    const auto when = OptionalString(object, QStringLiteral("when"));
    if (!when) {
        return std::unexpected(when.error());
    }
    Entry entry;
    entry.When = when->value_or(QString());
    entry.Args = object.value(QStringLiteral("args"));
    const QJsonValue command = object.value(QStringLiteral("command"));
    if (command.isString()) {
        entry.Command = command.toString();
        if (plugins.CommandOwner(*entry.Command) == nullptr) {
            return std::unexpected(
                QStringLiteral("no plugin has the command %1")
                    .arg(*entry.Command));
        }
    } else if (!command.isNull()) {
        return std::unexpected(
            QStringLiteral("\"command\" is neither a string nor null"));
    }
    const auto chord = ChooseKeyChord(*key, *mac, platform);
    if (!chord) {
        return std::unexpected(chord.error());
    }
    if (!*chord) {
        return OtherPlatform{};
    }
    entry.Chord = **chord;
    // A key that the terminal or a field of text needs (design.md §16).
    // A removal binds nothing, so it is not held to it.
    if (entry.Command && IsBareKey(entry.Chord) && entry.When.isEmpty()) {
        return std::unexpected(
            QStringLiteral("%1 has no Cmd, Ctrl or Alt: without \"when\" it "
                           "would take the text typed into any panel")
                .arg(WriteKeyChord(entry.Chord)));
    }
    return entry;
}

rpc::Result InvalidParams(const QString& message) {
    return Fail(rpc::EErrorCode::InvalidParams, message);
}

// The file cannot be read or written, or is not what a write may build on.
rpc::Result StoreFailed(const QString& message) {
    return std::unexpected(
        protocol::MakeError(protocol::EErrorCode::StoreFailed, message));
}

// The command, args and when that a request names.
struct Target {
    QString Command;
    QJsonValue Args;
    QString When;
};

// `args` of an entry or a request: none and null are the same.
QJsonValue ArgsOf(const QJsonValue& value) {
    return value.isNull() ? QJsonValue(QJsonValue::Undefined) : value;
}

// An entry of the file that binds the target: the command, args and when
// are the same. An entry that is not an object, or has a `when` that is not a
// string, is not.
bool BindsTarget(const QJsonValue& value, const Target& target) {
    if (!value.isObject()) {
        return false;
    }
    const QJsonObject object = value.toObject();
    const QJsonValue command = object.value(QStringLiteral("command"));
    const QJsonValue when = object.value(QStringLiteral("when"));
    if (!command.isString() || command.toString() != target.Command ||
        (!when.isUndefined() && !when.isString())) {
        return false;
    }
    return when.toString() == target.When &&
           ArgsOf(object.value(QStringLiteral("args"))) == target.Args;
}

// The target of keymap.setCommand and keymap.resetCommand; the message is for
// the caller.
std::expected<Target, QString> TargetOf(const QJsonObject& params) {
    const QJsonValue command = params.value(QStringLiteral("command"));
    const QJsonValue when = params.value(QStringLiteral("when"));
    if (!command.isString() || command.toString().isEmpty()) {
        return std::unexpected(
            QStringLiteral("\"command\" is not the id of a command"));
    }
    if (!when.isUndefined() && !when.isString()) {
        return std::unexpected(QStringLiteral("\"when\" is not a string"));
    }
    return Target{command.toString(),
                  ArgsOf(params.value(QStringLiteral("args"))),
                  when.toString()};
}

constexpr std::array<EPlatform, 2> PLATFORMS{EPlatform::Mac, EPlatform::Linux};

}  // namespace

KeymapService::KeymapService(QString configDirectory, Server& server,
                             const PluginRegistry& plugins, QObject* parent)
    : QObject(parent),
      Directory_(QDir::cleanPath(configDirectory)),
      Path_(Directory_ + QStringLiteral("/keymap.json")),
      Server_(server),
      Plugins_(plugins),
      Watcher_(this),
      Quiet_(this) {
    // The handler stays with the server; it finds the service gone, if it is.
    const QPointer<KeymapService> self(this);
    const bool getting = Server_.Register(
        QStringLiteral("keymap.get"),
        [self](const rpc::Call& call) -> QFuture<rpc::Result> {
            if (!self) {
                return rpc::Ready(
                    Fail(rpc::EErrorCode::InternalError,
                         QStringLiteral("the keymap is not available")));
            }
            const auto platform = PlatformOf(
                call.Params.toObject().value(QStringLiteral("platform")));
            if (!platform) {
                return rpc::Ready(Fail(
                    rpc::EErrorCode::InvalidParams,
                    QStringLiteral("keymap.get takes {platform}: \"mac\" or "
                                   "\"linux\"")));
            }
            const Table table = self->Resolve(*platform);
            const QJsonObject answer{
                {QStringLiteral("bindings"), table.Bindings},
                {QStringLiteral("problems"), table.Problems},
            };
            if (!Server::AnswerFitsAFrame(answer)) {
                return rpc::Ready(
                    Fail(rpc::EErrorCode::InternalError,
                         QStringLiteral("the keymap is too big for a frame")));
            }
            if (const auto connection = self->Server_.ConnectionOf(call.Peer)) {
                self->Asked_[*connection].insert(*platform);
            }
            return rpc::Ready(answer);
        });
    Q_ASSERT(getting);
    Q_UNUSED(getting);

    // Both change the file and leave the announcing to the watcher, as for a
    // hand edit (store.patch, in StoreService, is the precedent for the shape:
    // params checked first, the work done in the handler, which the event
    // loop runs to its end, so two writes never interleave).
    const bool setting = Server_.Register(
        QStringLiteral("keymap.setCommand"),
        [self](const rpc::Call& call) -> QFuture<rpc::Result> {
            return rpc::Ready(
                self ? self->SetCommand(call.Params.toObject())
                     : Fail(rpc::EErrorCode::InternalError,
                            QStringLiteral("the keymap is not available")));
        });
    const bool resetting = Server_.Register(
        QStringLiteral("keymap.resetCommand"),
        [self](const rpc::Call& call) -> QFuture<rpc::Result> {
            return rpc::Ready(
                self ? self->ResetCommand(call.Params.toObject())
                     : Fail(rpc::EErrorCode::InternalError,
                            QStringLiteral("the keymap is not available")));
        });
    Q_ASSERT(setting);
    Q_ASSERT(resetting);
    Q_UNUSED(setting);
    Q_UNUSED(resetting);

    connect(
        &Server_, &Server::ConnectionClosed, this,
        [this](Server::ConnectionId connection) { Asked_.erase(connection); });
    Quiet_.setSingleShot(true);
    Quiet_.setInterval(DEBOUNCE);
    connect(&Quiet_, &QTimer::timeout, this, &KeymapService::OnQuiet);
    connect(&Watcher_, &QFileSystemWatcher::fileChanged, this,
            &KeymapService::OnPathChanged);
    connect(&Watcher_, &QFileSystemWatcher::directoryChanged, this,
            &KeymapService::OnPathChanged);
    ReadFile();
    Sync();
}

KeymapService::~KeymapService() = default;

KeymapService::Signature KeymapService::SignatureOf(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists()) {
        return {};
    }
    return {true, info.size(), info.lastModified(), info.metadataChangeTime()};
}

void KeymapService::ReadFile() {
    QFile file(Path_);
    if (!file.exists()) {
        Entries_.reset();
        Problem_.clear();
        return;
    }
    const auto keep = [this](const QString& reason) {
        Problem_ = QStringLiteral("keymap.json is ignored: %1%2")
                       .arg(reason, Entries_ ? QStringLiteral("; the version "
                                                              "before it "
                                                              "stays")
                                             : QString());
    };
    if (!file.open(QIODevice::ReadOnly)) {
        keep(QStringLiteral("cannot read it: %1").arg(file.errorString()));
        return;
    }
    // One byte more than the limit tells a file that is over it.
    const QByteArray bytes = file.read(MAX_FILE_BYTES + 1);
    if (bytes.size() > MAX_FILE_BYTES) {
        keep(QStringLiteral("it is over 1 MiB"));
        return;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError) {
        keep(QStringLiteral("it is not JSON: %1 at byte %2")
                 .arg(error.errorString())
                 .arg(error.offset));
        return;
    }
    if (!document.isArray()) {
        keep(QStringLiteral("it is not an array"));
        return;
    }
    Entries_ = document.array();
    Problem_.clear();
}

KeymapService::Table KeymapService::Resolve(EPlatform platform) const {
    return ResolveWith(platform, Entries_, Problem_);
}

KeymapService::Table KeymapService::ResolveWith(
    EPlatform platform, const std::optional<QJsonArray>& entries,
    const QString& problem) const {
    // The registry chooses the plugins' keys for one platform, the one it is
    // made for; the window's may be the other.
    PluginRegistry registry(platform);
    for (const PluginManifest& plugin : Plugins_.Plugins()) {
        registry.Add(plugin);
    }
    Table table;
    for (const QString& found : registry.KeyProblems()) {
        table.Problems.append(found);
    }
    if (!problem.isEmpty()) {
        table.Problems.append(problem);
    }

    // In the order the keys came; a removed one leaves a hole.
    std::vector<std::optional<Binding>> bindings;
    QHash<QString, std::size_t> index;
    for (const KeyContribution& key : registry.KeyContributions()) {
        index.insert(KeyOf(key.Chord, key.When), bindings.size());
        bindings.push_back(
            Binding{key.Chord, key.When, key.Command, {}, key.Plugin});
    }
    if (entries) {
        for (qsizetype position = 0; position < entries->size(); ++position) {
            const auto entry =
                ParseEntry(entries->at(position), platform, Plugins_);
            if (!entry) {
                table.Problems.append(
                    QStringLiteral("keymap.json entry %1 is ignored: %2")
                        .arg(position)
                        .arg(entry.error()));
                continue;
            }
            if (std::holds_alternative<OtherPlatform>(*entry)) {
                continue;
            }
            const Entry& parsed = std::get<Entry>(*entry);
            const QString key = KeyOf(parsed.Chord, parsed.When);
            const auto taken = index.find(key);
            if (!parsed.Command) {
                if (taken != index.end()) {
                    bindings[*taken].reset();
                    index.erase(taken);
                }
                continue;
            }
            Binding binding{parsed.Chord, parsed.When, *parsed.Command,
                            parsed.Args, USER};
            if (taken != index.end()) {
                bindings[*taken] = std::move(binding);
            } else {
                index.insert(key, bindings.size());
                bindings.push_back(std::move(binding));
            }
        }
    }
    for (const auto& binding : bindings) {
        if (!binding) {
            continue;
        }
        QJsonObject object{
            {QStringLiteral("key"), WriteKeyChord(binding->Chord)},
            {QStringLiteral("command"), binding->Command},
            {QStringLiteral("source"), binding->Source},
        };
        if (!binding->Args.isUndefined() && !binding->Args.isNull()) {
            object.insert(QStringLiteral("args"), binding->Args);
        }
        if (!binding->When.isEmpty()) {
            object.insert(QStringLiteral("when"), binding->When);
        }
        table.Bindings.append(object);
    }
    return table;
}

namespace {

// The file as a write builds on it: what is in it now, not what the service
// has kept from before. A missing file is an empty array.
std::expected<QJsonArray, QString> ReadForWrite(const QString& path) {
    QFile file(path);
    if (!file.exists()) {
        return QJsonArray();
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(
            QStringLiteral("keymap.json cannot be read: %1, it is left as it "
                           "is")
                .arg(file.errorString()));
    }
    const QByteArray bytes = file.read(KeymapService::MAX_FILE_BYTES + 1);
    if (bytes.size() > KeymapService::MAX_FILE_BYTES) {
        return std::unexpected(
            QStringLiteral("keymap.json is over 1 MiB, it is left as it is"));
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError) {
        return std::unexpected(
            QStringLiteral("keymap.json is not JSON (%1 at byte %2), it is "
                           "left as it is: mend it by hand")
                .arg(error.errorString())
                .arg(error.offset));
    }
    if (!document.isArray()) {
        return std::unexpected(
            QStringLiteral("keymap.json is not an array, it is left as it "
                           "is: mend it by hand"));
    }
    return document.array();
}

// Replaces the file by `entries`, all or nothing. A directory that is not
// there is made as the store makes its own: for the owner only, and the file
// too (design.md §8).
rpc::Result WriteEntries(const QString& path, const QJsonArray& entries) {
    const QByteArray bytes =
        QJsonDocument(entries).toJson(QJsonDocument::Indented);
    if (bytes.size() > KeymapService::MAX_FILE_BYTES) {
        return StoreFailed(
            QStringLiteral("keymap.json would be over 1 MiB, it is left as "
                           "it is"));
    }
    const auto saved =
        SaveOwnerOnly(QFileInfo(path).absolutePath(), path, bytes);
    if (!saved) {
        return StoreFailed(
            QStringLiteral("cannot write keymap.json: %1").arg(saved.error()));
    }
    return QJsonValue();
}

// The bindings of a platform's table for the entries of the file (none:
// the plugins alone).
using Resolver =
    std::function<QJsonArray(EPlatform, const std::optional<QJsonArray>&)>;

// A binding of the table that is of the target: the command, args and when.
bool IsOfTarget(const QJsonObject& binding, const Target& target) {
    return binding.value(QStringLiteral("command")) == target.Command &&
           binding.value(QStringLiteral("when")).toString() == target.When &&
           ArgsOf(binding.value(QStringLiteral("args"))) == target.Args;
}

// The chords of the table's bindings that are of the target, by platform.
using Chords = std::array<std::vector<KeyChord>, 2>;

Chords ChordsOfTarget(const Resolver& resolve,
                      const std::optional<QJsonArray>& entries,
                      const Target& target) {
    Chords chords;
    for (std::size_t index = 0; index < PLATFORMS.size(); ++index) {
        for (const QJsonValue& value : resolve(PLATFORMS[index], entries)) {
            const QJsonObject binding = value.toObject();
            if (!IsOfTarget(binding, target)) {
                continue;
            }
            if (const auto chord = ParseKeyChord(
                    binding.value(QStringLiteral("key")).toString(),
                    PLATFORMS[index])) {
                chords[index].push_back(*chord);
            }
        }
    }
    return chords;
}

// The bindings of each platform's table that the plugins give alone.
using Contributions = std::array<QJsonArray, 2>;

// An entry with "command": null whose only effect, on each platform, is to
// remove contributed bindings of the target (and which removes one on some
// platform): it was written to unbind the target's own keys. What it removes
// is judged on the contributions alone: the table of the plugins with the
// entry, against the table without it. A key that another entry of the file
// holds does not matter: the removal is the same whether it is there or not.
// An entry that removes a binding of another command, or none of the target's,
// is not it, nor is one that removes the key of an entry above it in the file:
// the user wrote that.
bool UnbindsOnlyTarget(const Resolver& resolve,
                       const Contributions& contributions,
                       const QJsonValue& value, bool takesFileKey,
                       const Target& target) {
    if (takesFileKey || !value.isObject() ||
        !value.toObject().value(QStringLiteral("command")).isNull()) {
        return false;
    }
    bool removed = false;
    for (std::size_t index = 0; index < PLATFORMS.size(); ++index) {
        const QJsonArray with = resolve(PLATFORMS[index], QJsonArray{value});
        for (const QJsonValue& binding : contributions[index]) {
            if (with.contains(binding)) {
                continue;
            }
            if (!IsOfTarget(binding.toObject(), target)) {
                return false;
            }
            removed = true;
        }
    }
    return removed;
}

// The file without what a call for the target replaces: its own entries, and
// the entries that unbound its keys. One resolve of the plugins for each
// platform, then one pass over the file.
QJsonArray WithoutTarget(const Resolver& resolve, const PluginRegistry& plugins,
                         const QJsonArray& file, const Target& target) {
    Contributions contributions;
    for (std::size_t index = 0; index < PLATFORMS.size(); ++index) {
        contributions[index] = resolve(PLATFORMS[index], std::nullopt);
    }
    // The pairs (chord, when) that entries of the file above bind, by
    // platform.
    std::array<std::set<QString>, 2> above;
    QJsonArray kept;
    for (const QJsonValue& value : file) {
        if (BindsTarget(value, target)) {
            continue;
        }
        bool takesFileKey = false;
        std::array<std::optional<QString>, 2> keys;
        std::array<bool, 2> binds{};
        for (std::size_t index = 0; index < PLATFORMS.size(); ++index) {
            const auto parsed = ParseEntry(value, PLATFORMS[index], plugins);
            if (!parsed) {
                continue;
            }
            if (const auto* entry = std::get_if<Entry>(&*parsed)) {
                keys[index] = KeyOf(entry->Chord, entry->When);
                binds[index] = entry->Command.has_value();
                takesFileKey =
                    takesFileKey ||
                    (!binds[index] && above[index].contains(*keys[index]));
            }
        }
        if (!UnbindsOnlyTarget(resolve, contributions, value, takesFileKey,
                               target)) {
            kept.append(value);
        }
        for (std::size_t index = 0; index < PLATFORMS.size(); ++index) {
            if (keys[index] && binds[index]) {
                above[index].insert(*keys[index]);
            }
        }
    }
    return kept;
}

// A chord that no command is likely to have: an entry for Linux alone has to
// give the Mac something, and this takes nothing from it.
const QString DECOY_MAC_KEY = QStringLiteral("Cmd+Ctrl+Alt+Shift+F24");

// The entries "command": null that remove, on each platform, exactly the given
// chords of the target's contributions and nothing else. An entry with `key`
// removes it on Linux and, with no `mac`, on the Mac too; `mac` alone is for
// the Mac; there is no entry for Linux alone. So the mac and linux chords go
// in pairs, and a chord left over has its own entry: a Mac one with `mac`
// alone; a Linux one with `key` alone if the Mac has no binding for the same
// chord, else with `mac` set to a chord that nothing has. `takenOnMac` are the
// pairs (chord, when) that the plugins bind on the Mac.
std::expected<QJsonArray, QString> UnbindEntries(
    const Chords& chords, const QString& when,
    const std::set<QString>& takenOnMac) {
    const std::vector<KeyChord>& mac = chords[0];
    const std::vector<KeyChord>& linux = chords[1];
    QJsonArray entries;
    const auto add = [&](const QString& key, const QString& macKey) {
        QJsonObject entry{{QStringLiteral("command"), QJsonValue::Null}};
        if (!key.isEmpty()) {
            entry.insert(QStringLiteral("key"), key);
        }
        if (!macKey.isEmpty()) {
            entry.insert(QStringLiteral("mac"), macKey);
        }
        if (!when.isEmpty()) {
            entry.insert(QStringLiteral("when"), when);
        }
        entries.append(entry);
    };
    const std::size_t paired = std::min(mac.size(), linux.size());
    for (std::size_t i = 0; i < paired; ++i) {
        add(WriteKeyChord(linux[i]), WriteKeyChord(mac[i]));
    }
    for (std::size_t i = paired; i < mac.size(); ++i) {
        add(QString(), WriteKeyChord(mac[i]));
    }
    for (std::size_t i = paired; i < linux.size(); ++i) {
        const QString key = WriteKeyChord(linux[i]);
        const auto asked = ChooseKeyChord(key, std::nullopt, EPlatform::Mac);
        if (asked && *asked && !takenOnMac.contains(KeyOf(**asked, when))) {
            add(key, QString());
            continue;
        }
        const auto decoy = ParseKeyChord(DECOY_MAC_KEY, EPlatform::Mac);
        if (!decoy || takenOnMac.contains(KeyOf(*decoy, when))) {
            return std::unexpected(
                QStringLiteral("%1 cannot be removed on Linux without taking "
                               "the same key of another command on the Mac")
                    .arg(key));
        }
        add(key, DECOY_MAC_KEY);
    }
    return entries;
}

// The pairs (chord, when) of the bindings, as keys of a set.
std::set<QString> PairsOf(const QJsonArray& bindings, EPlatform platform) {
    std::set<QString> pairs;
    for (const QJsonValue& value : bindings) {
        const QJsonObject binding = value.toObject();
        if (const auto chord = ParseKeyChord(
                binding.value(QStringLiteral("key")).toString(), platform)) {
            pairs.insert(KeyOf(
                *chord, binding.value(QStringLiteral("when")).toString()));
        }
    }
    return pairs;
}

}  // namespace

rpc::Result KeymapService::SetCommand(const QJsonObject& params) {
    const auto target = TargetOf(params);
    const QJsonValue given = params.value(QStringLiteral("entries"));
    if (!target || !given.isArray()) {
        return InvalidParams(QStringLiteral(
            "keymap.setCommand takes {command, args?, when?, entries}: "
            "entries is an array of all the keys of the command, each "
            "{key?, mac?}; an empty array removes them all"));
    }
    if (Plugins_.CommandOwner(target->Command) == nullptr) {
        return InvalidParams(QStringLiteral("no plugin has the command %1")
                                 .arg(target->Command));
    }
    // The keys asked for, as entries of the file; each one is checked as the
    // file's own are, for both platforms: the file is shared.
    struct Wanted {
        QJsonObject Entry;
        std::array<std::optional<KeyChord>, 2> Chords;
    };
    std::vector<Wanted> wanted;
    for (const QJsonValue& value : given.toArray()) {
        const std::size_t position = wanted.size();
        if (!value.isObject()) {
            return InvalidParams(
                QStringLiteral("entry %1 is not an object").arg(position));
        }
        QJsonObject entry = value.toObject();
        for (const QString& name : entry.keys()) {
            if (name != QStringLiteral("key") &&
                name != QStringLiteral("mac")) {
                return InvalidParams(
                    QStringLiteral("entry %1 has \"%2\": an entry has only "
                                   "\"key\" and \"mac\", the command, args and "
                                   "when are those of the request")
                        .arg(position)
                        .arg(name));
            }
        }
        entry.insert(QStringLiteral("command"), target->Command);
        if (!target->Args.isUndefined()) {
            entry.insert(QStringLiteral("args"), target->Args);
        }
        if (!target->When.isEmpty()) {
            entry.insert(QStringLiteral("when"), target->When);
        }
        Wanted item{entry, {}};
        for (std::size_t index = 0; index < PLATFORMS.size(); ++index) {
            const auto parsed = ParseEntry(entry, PLATFORMS[index], Plugins_);
            if (!parsed) {
                return InvalidParams(QStringLiteral("entry %1 is refused: %2")
                                         .arg(position)
                                         .arg(parsed.error()));
            }
            if (const auto* own = std::get_if<Entry>(&*parsed)) {
                item.Chords[index] = own->Chord;
            }
        }
        wanted.push_back(std::move(item));
    }

    const auto current = ReadForWrite(Path_);
    if (!current) {
        return StoreFailed(current.error());
    }
    const Resolver resolve = [this](EPlatform platform,
                                    const std::optional<QJsonArray>& entries) {
        return ResolveWith(platform, entries, QString()).Bindings;
    };
    const QJsonArray kept = WithoutTarget(resolve, Plugins_, *current, *target);

    // What the plugins give the command, for each platform (a key that lost
    // a conflict to another plugin is not there), whatever the file has: a
    // key that another entry of the file holds now is the command's again
    // when that entry goes. Plugins give no args.
    Chords contributed;
    if (target->Args.isUndefined()) {
        contributed = ChordsOfTarget(resolve, std::nullopt, *target);
    }
    std::array<std::set<QString>, 2> contributedPairs;
    std::array<std::set<QString>, 2> wantedPairs;
    for (std::size_t index = 0; index < PLATFORMS.size(); ++index) {
        for (const KeyChord& chord : contributed[index]) {
            contributedPairs[index].insert(KeyOf(chord, target->When));
        }
        for (const Wanted& item : wanted) {
            if (item.Chords[index]) {
                wantedPairs[index].insert(
                    KeyOf(*item.Chords[index], target->When));
            }
        }
    }
    // A key that the plugins already give is not written again. An entry
    // is that on each platform it applies to.
    QJsonArray written;
    std::vector<const Wanted*> byPlugins;
    for (const Wanted& item : wanted) {
        bool contributedEverywhere = true;
        for (std::size_t index = 0; index < PLATFORMS.size(); ++index) {
            contributedEverywhere = contributedEverywhere &&
                                    (!item.Chords[index] ||
                                     contributedPairs[index].contains(KeyOf(
                                         *item.Chords[index], target->When)));
        }
        if (contributedEverywhere) {
            byPlugins.push_back(&item);
        } else {
            written.append(item.Entry);
        }
    }
    // The keys that the plugins give and the command must not have.
    Chords removed;
    for (std::size_t index = 0; index < PLATFORMS.size(); ++index) {
        for (const KeyChord& chord : contributed[index]) {
            if (!wantedPairs[index].contains(KeyOf(chord, target->When))) {
                removed[index].push_back(chord);
            }
        }
    }
    const auto unbinds = UnbindEntries(
        removed, target->When,
        PairsOf(resolve(EPlatform::Mac, std::nullopt), EPlatform::Mac));
    if (!unbinds) {
        return InvalidParams(unbinds.error());
    }
    // The removals come first: they take the plugins' keys only, and not the
    // keys that other entries of the file bind below them.
    QJsonArray result = *unbinds;
    for (const QJsonValue& value : kept) {
        result.append(value);
    }
    for (const QJsonValue& value : written) {
        result.append(value);
    }
    // A key that the plugins give and that another entry of the file holds
    // is not the command's: write it, and it wins as the last entry.
    if (!byPlugins.empty()) {
        const Chords have = ChordsOfTarget(resolve, result, *target);
        std::array<std::set<QString>, 2> havePairs;
        for (std::size_t index = 0; index < PLATFORMS.size(); ++index) {
            for (const KeyChord& chord : have[index]) {
                havePairs[index].insert(KeyOf(chord, target->When));
            }
        }
        for (const Wanted* item : byPlugins) {
            bool held = true;
            for (std::size_t index = 0; index < PLATFORMS.size(); ++index) {
                held = held && (!item->Chords[index] ||
                                havePairs[index].contains(
                                    KeyOf(*item->Chords[index], target->When)));
            }
            if (!held) {
                result.append(item->Entry);
            }
        }
    }
    // Nothing to change: the file, or the lack of it, stays as it is.
    return result == *current ? rpc::Result(QJsonValue())
                              : WriteEntries(Path_, result);
}

rpc::Result KeymapService::ResetCommand(const QJsonObject& params) {
    const auto target = TargetOf(params);
    if (!target) {
        return InvalidParams(
            QStringLiteral("keymap.resetCommand takes {command, args?, when?}: "
                           "%1")
                .arg(target.error()));
    }
    const auto current = ReadForWrite(Path_);
    if (!current) {
        return StoreFailed(current.error());
    }
    const Resolver resolve = [this](EPlatform platform,
                                    const std::optional<QJsonArray>& entries) {
        return ResolveWith(platform, entries, QString()).Bindings;
    };
    const QJsonArray kept = WithoutTarget(resolve, Plugins_, *current, *target);
    return kept == *current ? rpc::Result(QJsonValue())
                            : WriteEntries(Path_, kept);
}

void KeymapService::Sync() {
    // A config directory that is not made yet is watched by the nearest
    // directory above it that is; its event leads here again.
    QString directory = Directory_;
    while (!QFileInfo(directory).isDir()) {
        const QString parent = QDir::cleanPath(directory + u"/..");
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    QStringList wanted;
    if (QFileInfo(directory).isDir()) {
        wanted.append(directory);
    }
    if (QFileInfo(Path_).isFile()) {
        wanted.append(Path_);
    }

    // What Qt watches now is what it has kept: a file replaced by rename is
    // gone from it.
    QStringList stale;
    QSet<QString> have;
    for (const QStringList& list : {Watcher_.files(), Watcher_.directories()}) {
        for (const QString& path : list) {
            have.insert(path);
            if (!wanted.contains(path)) {
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
        it = wanted.contains(it.key()) ? std::next(it) : Seen_.erase(it);
    }
    if (missing.isEmpty()) {
        return;
    }
    const QStringList failed = Watcher_.addPaths(missing);
    if (!failed.isEmpty()) {
        qWarning("roostd: cannot watch %s", qUtf8Printable(failed.join(u", ")));
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
            Quiet_.start();
        }
    }
}

void KeymapService::OnPathChanged(const QString& path) {
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
    Quiet_.start();
}

void KeymapService::OnQuiet() {
    // A file that appeared since the last event, a directory that came back.
    Sync();
    const auto before = std::make_pair(Entries_, Problem_);
    // The signatures come before the read: an edit that lands after them is
    // not taken as seen, its event finds the difference. The signature has no
    // access time, so the read does not change it.
    for (auto it = Seen_.begin(); it != Seen_.end(); ++it) {
        it.value() = SignatureOf(it.key());
    }
    ReadFile();
    if (std::make_pair(Entries_, Problem_) != before) {
        PublishAll();
    }
}

void KeymapService::PublishAll() {
    std::set<EPlatform> platforms;
    for (const auto& [connection, asked] : Asked_) {
        platforms.insert(asked.begin(), asked.end());
    }
    for (const EPlatform platform : platforms) {
        const Table table = Resolve(platform);
        const QJsonObject payload{
            {QStringLiteral("platform"), PlatformName(platform)},
            {QStringLiteral("bindings"), table.Bindings},
            {QStringLiteral("problems"), table.Problems},
        };
        if (!Server::EventFitsAFrame(CHANGED, payload)) {
            qWarning(
                "roostd: the keymap for %s is too big for a frame and is "
                "not published",
                qUtf8Printable(PlatformName(platform)));
            continue;
        }
        Server_.Bus().Publish(CHANGED, payload);
    }
}

}  // namespace roost
