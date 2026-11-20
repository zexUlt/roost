#include "roost/palette_model.hpp"

#include <QJSEngine>
#include <QJsonArray>
#include <QQmlEngine>
#include <QStringList>

#include <optional>
#include <utility>

#include "api/keymap_service.hpp"
#include "qml_api/roost_singleton.hpp"
#include "roost/shell.hpp"

namespace roost {

using namespace Qt::StringLiterals;

namespace {

const QString USER = QStringLiteral("user");

// One entry of the keymap table, its key parsed.
struct TableKey {
    KeyChord Chord;
    // The key as the table writes it.
    QString Text;
    QString When;
    QString Command;
    QString Source;
};

// The pair that a key is in the table: the chord in the form that keys are
// compared in, and the panel type, which cannot hold a NUL.
QString PairOf(const KeyChord& chord, const QString& when) {
    return WriteKeyChord(api::MatchForm(chord)) + QChar(u'\0') + when;
}

// A key as a person reads it.
QString Describe(const KeyChord& chord, const QString& when) {
    const QString text = WriteKeyChord(chord);
    return when.isEmpty() ? text
                          : QStringLiteral("%1 (when %2)").arg(text, when);
}

bool IsLetter(const QString& key) {
    return key.size() == 1 && key.front().isLetter();
}

// Whether the key is one that the terminal needs (design.md §16, "Ввод
// текста"): Ctrl with a letter is a control character for the program, and
// Alt with a letter types a character on macOS. Only a key that is active in
// the terminal can take it from there.
bool CapturesInput(const TableKey& key, EPlatform platform) {
    if (!key.When.isEmpty() &&
        key.When != QString::fromLatin1(Shell::TERMINAL_PANEL)) {
        return false;
    }
    if (!IsLetter(key.Chord.Key)) {
        return false;
    }
    return key.Chord.Ctrl || (platform == EPlatform::Mac && key.Chord.Alt);
}

void AddSentence(QString& all, const QString& sentence) {
    if (!all.isEmpty()) {
        all += QStringLiteral("; ");
    }
    all += sentence;
}

}  // namespace

PaletteModel::PaletteModel(QObject* parent) : QAbstractListModel(parent) {}

std::vector<PaletteModel::Entry> PaletteModel::BuildEntries(
    const QJsonObject& plugins, const QJsonObject& keymap, EPlatform platform) {
    // The table, by pair and by command.
    std::vector<TableKey> table;
    QHash<QString, int> byPair;
    QHash<QString, QList<int>> byCommand;
    for (const QJsonValue& value : keymap.value(u"bindings"_s).toArray()) {
        const QJsonObject binding = value.toObject();
        const QString text = binding.value(u"key"_s).toString();
        const auto chord = ParseKeyChord(text, platform);
        const QString command = binding.value(u"command"_s).toString();
        if (!chord || command.isEmpty()) {
            continue;
        }
        const int index = static_cast<int>(table.size());
        table.push_back(
            TableKey{.Chord = *chord,
                     .Text = text,
                     .When = binding.value(u"when"_s).toString(),
                     .Command = command,
                     .Source = binding.value(u"source"_s).toString()});
        byPair.insert(PairOf(*chord, table.back().When), index);
        byCommand[command].append(index);
    }

    // The commands, in the order of the plugins and then of the manifest.
    std::vector<Entry> entries;
    QHash<QString, int> byId;
    const QJsonArray pluginList = plugins.value(u"plugins"_s).toArray();
    for (const QJsonValue& value : pluginList) {
        const QJsonObject plugin = value.toObject();
        const QString pluginId = plugin.value(u"id"_s).toString();
        const QJsonArray commands = plugin.value(u"contributes"_s)
                                        .toObject()
                                        .value(u"commands"_s)
                                        .toArray();
        for (const QJsonValue& item : commands) {
            const QJsonObject command = item.toObject();
            const QString id = command.value(u"id"_s).toString();
            if (id.isEmpty() || byId.contains(id)) {
                continue;
            }
            Entry entry;
            entry.CommandId = id;
            entry.Title = command.value(u"title"_s).toString(id);
            entry.Plugin = pluginId;
            byId.insert(id, static_cast<int>(entries.size()));
            entries.push_back(std::move(entry));
        }
    }

    // What the table says of each command's keys.
    for (Entry& entry : entries) {
        QStringList keys;
        QStringList whens;
        for (const int index : byCommand.value(entry.CommandId)) {
            const TableKey& key = table[static_cast<std::size_t>(index)];
            keys.append(key.Text);
            if (!whens.contains(key.When)) {
                whens.append(key.When);
            }
            entry.Overridden = entry.Overridden || key.Source == USER;
            entry.CapturesInput =
                entry.CapturesInput || CapturesInput(key, platform);
        }
        entry.Key = keys.join(QStringLiteral(", "));
        whens.removeAll(QString());
        entry.When = whens.join(QStringLiteral(", "));
    }

    // The keys that the manifests ask for, against the table: the loser of a
    // conflict and the key that keymap.json took are not in the table under
    // their command, so they are found from the manifest.
    for (const QJsonValue& value : pluginList) {
        const QJsonObject plugin = value.toObject();
        const QString pluginId = plugin.value(u"id"_s).toString();
        const QJsonArray bindings = plugin.value(u"contributes"_s)
                                        .toObject()
                                        .value(u"keybindings"_s)
                                        .toArray();
        for (const QJsonValue& item : bindings) {
            const QJsonObject binding = item.toObject();
            const QString command = binding.value(u"command"_s).toString();
            const auto own = byId.constFind(command);
            if (own == byId.constEnd()) {
                continue;
            }
            const auto stringOf =
                [&binding](const QString& name) -> std::optional<QString> {
                const QJsonValue field = binding.value(name);
                if (!field.isString()) {
                    return std::nullopt;
                }
                return field.toString();
            };
            const auto chord = ChooseKeyChord(stringOf(u"key"_s),
                                              stringOf(u"mac"_s), platform);
            if (!chord || !*chord) {
                continue;
            }
            const QString when = binding.value(u"when"_s).toString();
            const auto taken = byPair.constFind(PairOf(**chord, when));
            if (taken == byPair.constEnd()) {
                continue;
            }
            const TableKey& holder = table[static_cast<std::size_t>(*taken)];
            if (holder.Source == pluginId) {
                continue;
            }
            Entry& mine = entries[static_cast<std::size_t>(*own)];
            if (holder.Source == USER) {
                mine.Overridden = true;
                continue;
            }
            const QString sentence =
                QStringLiteral(
                    "%1 is bound by %2 and %3: %2 keeps it, %3 is "
                    "ignored")
                    .arg(Describe(**chord, when), holder.Source, pluginId);
            AddSentence(mine.Conflict, sentence);
            if (const auto winner = byId.constFind(holder.Command);
                winner != byId.constEnd()) {
                AddSentence(entries[static_cast<std::size_t>(*winner)].Conflict,
                            sentence);
            }
        }
    }
    return entries;
}

bool PaletteModel::Matches(const QString& pattern, const QString& text) {
    const QString wanted = pattern.toLower();
    const QString haystack = text.toLower();
    qsizetype next = 0;
    for (const QChar letter : wanted) {
        next = haystack.indexOf(letter, next);
        if (next < 0) {
            return false;
        }
        ++next;
    }
    return true;
}

void PaletteModel::SetApi(api::Api* api) {
    for (const QMetaObject::Connection& connection : Connections_) {
        disconnect(connection);
    }
    Connections_.clear();
    Api_ = api;
    if (Api_ != nullptr) {
        Connections_.append(connect(&Api_->Plugins(),
                                    &api::PluginService::Changed, this,
                                    &PaletteModel::Rebuild));
        Connections_.append(connect(&Api_->Keymap(),
                                    &api::KeymapService::Changed, this,
                                    &PaletteModel::Rebuild));
    }
    Rebuild();
}

void PaletteModel::componentComplete() {
    if (Api_ != nullptr) {
        return;
    }
    QQmlEngine* engine = qmlEngine(this);
    if (engine == nullptr) {
        return;
    }
    auto* singleton =
        engine->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
    if (singleton != nullptr && singleton->api() != nullptr) {
        SetApi(singleton->api());
    }
}

int PaletteModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(Shown_.size());
}

QVariant PaletteModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(Shown_.size())) {
        return {};
    }
    const Entry& entry = Entries_[static_cast<std::size_t>(
        Shown_[static_cast<std::size_t>(index.row())])];
    switch (role) {
        case CommandIdRole:
            return entry.CommandId;
        case TitleRole:
            return entry.Title;
        case PluginRole:
            return entry.Plugin;
        case KeyRole:
            return entry.Key;
        case WhenRole:
            return entry.When;
        case ConflictRole:
            return entry.Conflict;
        case OverriddenRole:
            return entry.Overridden;
        case CapturesInputRole:
            return entry.CapturesInput;
        default:
            return {};
    }
}

QHash<int, QByteArray> PaletteModel::roleNames() const {
    return {
        {CommandIdRole, "commandId"},
        {TitleRole, "title"},
        {PluginRole, "plugin"},
        {KeyRole, "key"},
        {WhenRole, "when"},
        {ConflictRole, "conflict"},
        {OverriddenRole, "overridden"},
        {CapturesInputRole, "capturesInput"},
    };
}

void PaletteModel::setFilter(const QString& filter) {
    if (filter == Filter_) {
        return;
    }
    beginResetModel();
    Filter_ = filter;
    Shown_ = Shown();
    endResetModel();
    emit filterChanged();
}

QString PaletteModel::commandAt(int row) const {
    if (row < 0 || row >= static_cast<int>(Shown_.size())) {
        return {};
    }
    return Entries_[static_cast<std::size_t>(
                        Shown_[static_cast<std::size_t>(row)])]
        .CommandId;
}

void PaletteModel::Rebuild() {
    const int before = total();
    beginResetModel();
    if (Api_ != nullptr) {
        Entries_ = BuildEntries(Api_->Plugins().Plugins(),
                                Api_->Keymap().Table(), HostPlatform());
    } else {
        Entries_.clear();
    }
    Shown_ = Shown();
    endResetModel();
    if (total() != before) {
        emit totalChanged();
    }
}

std::vector<int> PaletteModel::Shown() const {
    std::vector<int> shown;
    for (std::size_t index = 0; index < Entries_.size(); ++index) {
        const Entry& entry = Entries_[index];
        if (Matches(Filter_, entry.Title) ||
            Matches(Filter_, entry.CommandId)) {
            shown.push_back(static_cast<int>(index));
        }
    }
    return shown;
}

}  // namespace roost
