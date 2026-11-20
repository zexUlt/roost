#include "roostd/keymap_service.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QtLogging>

#include <chrono>
#include <variant>
#include <vector>

#include "core/rpc.hpp"
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

bool IsFunctionKey(const QString& key) {
    bool numbered = false;
    const int number = key.mid(1).toInt(&numbered);
    return key.startsWith(u'F') && numbered && number >= 1 && number <= 24;
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
    if (entry.Command && !entry.Chord.Cmd && !entry.Chord.Ctrl &&
        !entry.Chord.Alt && !IsFunctionKey(entry.Chord.Key) &&
        entry.When.isEmpty()) {
        return std::unexpected(
            QStringLiteral("%1 has no Cmd, Ctrl or Alt: without \"when\" it "
                           "would take the text typed into any panel")
                .arg(WriteKeyChord(entry.Chord)));
    }
    return entry;
}

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
    // The registry chooses the plugins' keys for one platform, the one it is
    // made for; the window's may be the other.
    PluginRegistry registry(platform);
    for (const PluginManifest& plugin : Plugins_.Plugins()) {
        registry.Add(plugin);
    }
    Table table;
    for (const QString& problem : registry.KeyProblems()) {
        table.Problems.append(problem);
    }
    if (!Problem_.isEmpty()) {
        table.Problems.append(Problem_);
    }

    // In the order the keys came; a removed one leaves a hole.
    std::vector<std::optional<Binding>> bindings;
    QHash<QString, std::size_t> index;
    for (const KeyContribution& key : registry.KeyContributions()) {
        index.insert(KeyOf(key.Chord, key.When), bindings.size());
        bindings.push_back(
            Binding{key.Chord, key.When, key.Command, {}, key.Plugin});
    }
    if (Entries_) {
        for (qsizetype position = 0; position < Entries_->size(); ++position) {
            const auto entry =
                ParseEntry(Entries_->at(position), platform, Plugins_);
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
