#include "api/keymap_service.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QtLogging>

#include <utility>

#include "api/thread_check.hpp"
#include "terminal/physical_keys.hpp"

namespace roost::api {

namespace {

const QString CHANGED = QStringLiteral("keymap.changed");
const QString PLATFORM = QStringLiteral("platform");
const QString BINDINGS = QStringLiteral("bindings");
const QString PROBLEMS = QStringLiteral("problems");

// The pair as a hash key: the chord in its normal text and the panel type,
// which cannot hold a NUL.
QString IndexKey(const KeyChord& chord, const QString& when) {
    return WriteKeyChord(chord) + QChar(u'\0') + when;
}

}  // namespace

KeyChord MatchForm(KeyChord chord) {
    if (!chord.Shift || chord.Key.size() != 1) {
        return chord;
    }
    const int code = chord.Key.front().unicode();
    for (const SymbolKey& symbol : SymbolKeys()) {
        if (code == symbol.Unshifted) {
            chord.Key = QString(QChar(symbol.Shifted));
            chord.Shift = false;
            return chord;
        }
        if (code == symbol.Shifted) {
            chord.Shift = false;
            return chord;
        }
    }
    return chord;
}

KeymapService::KeymapService(IoThread& io, BusService& bus, QObject* parent)
    : QObject(parent), Io_(io), Bus_(bus) {
    connect(&Io_, &IoThread::Disconnected, this,
            &KeymapService::OnDisconnected);
}

QString KeymapService::PlatformName() {
    return HostPlatform() == EPlatform::Mac ? QStringLiteral("mac")
                                            : QStringLiteral("linux");
}

void KeymapService::Start() {
    if (!CheckThread(*this, "KeymapService::Start") || Started_) {
        return;
    }
    Started_ = true;
    Subscribe();
    static_cast<void>(Fetch());
}

QFuture<rpc::Result> KeymapService::Get() {
    if (!CheckThread(*this, "KeymapService::Get")) {
        return rpc::Ready(std::unexpected(WrongThread()));
    }
    if (Loaded_) {
        return rpc::Ready(Table_);
    }
    return Fetch();
}

namespace {

// The params that name the entries of one command.
QJsonObject TargetParams(const QString& command, const QString& when,
                         const QJsonValue& args) {
    QJsonObject params{{QStringLiteral("command"), command}};
    if (!when.isEmpty()) {
        params.insert(QStringLiteral("when"), when);
    }
    if (!args.isUndefined()) {
        params.insert(QStringLiteral("args"), args);
    }
    return params;
}

}  // namespace

QFuture<rpc::Result> KeymapService::SetCommand(const QString& command,
                                               const QJsonArray& entries,
                                               const QString& when,
                                               const QJsonValue& args) {
    if (!CheckThread(*this, "KeymapService::SetCommand")) {
        return rpc::Ready(std::unexpected(WrongThread()));
    }
    QJsonObject params = TargetParams(command, when, args);
    params.insert(QStringLiteral("entries"), entries);
    return Io_.Request(QStringLiteral("keymap.setCommand"), params);
}

QFuture<rpc::Result> KeymapService::ResetCommand(const QString& command,
                                                 const QString& when,
                                                 const QJsonValue& args) {
    if (!CheckThread(*this, "KeymapService::ResetCommand")) {
        return rpc::Ready(std::unexpected(WrongThread()));
    }
    return Io_.Request(QStringLiteral("keymap.resetCommand"),
                       TargetParams(command, when, args));
}

bool KeymapService::IsLoaded() const {
    return CheckThread(*this, "KeymapService::IsLoaded") && Loaded_;
}

const KeyBinding* KeymapService::Find(const KeyChord& chord,
                                      const QString& when) const {
    if (!CheckThread(*this, "KeymapService::Find")) {
        return nullptr;
    }
    const auto found = Index_.constFind(IndexKey(chord, when));
    return found == Index_.cend() ? nullptr : &*found;
}

void KeymapService::Subscribe() {
    if (Subscription_ != 0) {
        return;
    }
    const auto subscription = Bus_.Subscribe(
        CHANGED, this,
        [this](const QString& /*name*/, const QJsonValue& payload) {
            OnChanged(payload);
        });
    if (subscription) {
        Subscription_ = *subscription;
    }
}

QFuture<rpc::Result> KeymapService::Fetch() {
    // Decided now: an answer is kept only if the subscription was asked for
    // before the request, on the same socket, so that roostd hands out the
    // table and the events that follow it in this order.
    const bool tracked = Started_;
    return Io_
        .Request(QStringLiteral("keymap.get"),
                 QJsonObject{{PLATFORM, PlatformName()}})
        .then(this, [this, tracked, epoch = Epoch_](rpc::Result answer) {
            if (answer && tracked && epoch == Epoch_ && answer->isObject()) {
                Load(answer->toObject());
            }
            return answer;
        });
}

void KeymapService::Load(const QJsonObject& table) {
    QHash<QString, KeyBinding> index;
    for (const QJsonValue& value : table.value(BINDINGS).toArray()) {
        const QJsonObject binding = value.toObject();
        const QString command =
            binding.value(QStringLiteral("command")).toString();
        const auto chord = ParseKeyChord(
            binding.value(QStringLiteral("key")).toString(), HostPlatform());
        if (!chord || command.isEmpty()) {
            qWarning(
                "keymap: a binding of roostd is not used: %s",
                qUtf8Printable(QString::fromUtf8(
                    QJsonDocument(binding).toJson(QJsonDocument::Compact))));
            continue;
        }
        KeyBinding parsed{
            .Chord = MatchForm(*chord),
            .When = binding.value(QStringLiteral("when")).toString(),
            .Command = command,
            .Args = binding.value(QStringLiteral("args")),
            .Source = binding.value(QStringLiteral("source")).toString()};
        // Two spellings of one key (Ctrl++ and Ctrl+Shift+=) are one pair: the
        // first in the table keeps it.
        const QString key = IndexKey(parsed.Chord, parsed.When);
        if (!index.contains(key)) {
            index.insert(key, std::move(parsed));
        }
    }
    Table_ = QJsonObject{{BINDINGS, table.value(BINDINGS)},
                         {PROBLEMS, table.value(PROBLEMS)}};
    Index_ = std::move(index);
    Loaded_ = true;
    emit Changed();
}

void KeymapService::OnChanged(const QJsonValue& payload) {
    const QJsonObject object = payload.toObject();
    if (object.value(PLATFORM).toString() != PlatformName()) {
        return;
    }
    Load(object);
}

void KeymapService::OnDisconnected() {
    ++Epoch_;
    Started_ = false;
    Bus_.Unsubscribe(std::exchange(Subscription_, 0));
    const bool wasLoaded = std::exchange(Loaded_, false);
    Table_ = {};
    Index_.clear();
    if (wasLoaded) {
        emit Changed();
    }
}

}  // namespace roost::api
