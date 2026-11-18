#include "sessions/model/list_state.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

#include <algorithm>

namespace roost::sessions {

namespace {

const QString WORKING = QStringLiteral("working");

const QString VERSION_KEY = QStringLiteral("version");
const QString CATEGORIES_KEY = QStringLiteral("categories");
const QString NAMES_KEY = QStringLiteral("names");
const QString ARCHIVED_KEY = QStringLiteral("archived");
const QString COLORS_KEY = QStringLiteral("colors");
const QString PINNED_KEY = QStringLiteral("pinned");
const QString FOLDED_KEY = QStringLiteral("folded");
const QString UNREAD_KEY = QStringLiteral("unread");
const QString STATES_KEY = QStringLiteral("states");
const QString VIEW_KEY = QStringLiteral("view");

const QString QUERY_KEY = QStringLiteral("query");
const QString TAB_KEY = QStringLiteral("tab");
const QString ARCHIVE_KEY = QStringLiteral("archive");
const QString COMPACT_KEY = QStringLiteral("compact");
const QString OPENED_KEY = QStringLiteral("opened");

const QString TAB_ALL = QStringLiteral("all");
const QString TAB_ACTIVE = QStringLiteral("active");
const QString TAB_UNREAD = QStringLiteral("unread");

// "agent/id" with both parts present.
bool IsSessionKey(const QString& key) {
    const qsizetype slash = key.indexOf(QLatin1Char('/'));
    return slash > 0 && slash < key.size() - 1;
}

QStringList Sorted(const QSet<QString>& set) {
    QStringList list(set.begin(), set.end());
    list.sort();
    return list;
}

QJsonArray ToArray(const QStringList& list) {
    QJsonArray array;
    for (const QString& item : list) {
        array.append(item);
    }
    return array;
}

// {session key: non-empty string}; anything else in the object is dropped.
QHash<QString, QString> ReadSessionStrings(const QJsonValue& value) {
    QHash<QString, QString> out;
    const QJsonObject object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (IsSessionKey(it.key()) && it.value().isString() &&
            !it.value().toString().isEmpty()) {
            out.insert(it.key(), it.value().toString());
        }
    }
    return out;
}

QJsonObject WriteStrings(const QHash<QString, QString>& map) {
    QJsonObject out;
    for (auto it = map.begin(); it != map.end(); ++it) {
        out.insert(it.key(), it.value());
    }
    return out;
}

// Strings of an array, in order; other values are dropped.
QStringList ReadStrings(const QJsonValue& value) {
    QStringList out;
    for (const QJsonValue& item : value.toArray()) {
        if (item.isString()) {
            out << item.toString();
        }
    }
    return out;
}

QSet<QString> ReadSessionKeys(const QJsonValue& value) {
    QSet<QString> out;
    for (const QString& item : ReadStrings(value)) {
        if (IsSessionKey(item)) {
            out.insert(item);
        }
    }
    return out;
}

QString TabName(ETab tab) {
    switch (tab) {
        case ETab::Active:
            return TAB_ACTIVE;
        case ETab::Unread:
            return TAB_UNREAD;
        case ETab::All:
            break;
    }
    return TAB_ALL;
}

ETab TabOf(const QString& name) {
    if (name == TAB_ACTIVE) {
        return ETab::Active;
    }
    if (name == TAB_UNREAD) {
        return ETab::Unread;
    }
    return ETab::All;
}

template <typename TValue>
bool Assign(TValue& target, const TValue& value) {
    if (target == value) {
        return false;
    }
    target = value;
    return true;
}

// Each item once, at its last position.
QStringList DedupedKeepLast(const QStringList& list) {
    QStringList reversed;
    QSet<QString> seen;
    for (auto it = list.crbegin(); it != list.crend(); ++it) {
        if (!seen.contains(*it)) {
            seen.insert(*it);
            reversed << *it;
        }
    }
    return QStringList(reversed.crbegin(), reversed.crend());
}

bool Toggle(QSet<QString>& set, const QString& item) {
    if (!set.remove(item)) {
        set.insert(item);
    }
    return true;
}

}  // namespace

bool ListState::Restore(const QJsonObject& document) {
    const QSet<QString> shownBefore = ShownUnread();
    Categories_ = ReadSessionStrings(document.value(CATEGORIES_KEY));
    Names_ = ReadSessionStrings(document.value(NAMES_KEY));
    Archived_ = ReadSessionKeys(document.value(ARCHIVED_KEY));
    Unread_ = ReadSessionKeys(document.value(UNREAD_KEY));
    States_ = ReadSessionStrings(document.value(STATES_KEY));
    // An object of nothing but malformed entries is not a seeded one.
    Seeded_ =
        document.value(STATES_KEY).isObject() &&
        (document.value(STATES_KEY).toObject().isEmpty() || !States_.isEmpty());

    Colors_.clear();
    const QJsonObject colors = document.value(COLORS_KEY).toObject();
    for (auto it = colors.begin(); it != colors.end(); ++it) {
        const int index = it.value().toInt(-1);
        if (!it.key().isEmpty() && index >= 0 && index < COLOR_COUNT) {
            Colors_.insert(it.key(), index);
        }
    }

    View_ = ViewState();
    View_.Pinned = DedupedKeepLast(ReadStrings(document.value(PINNED_KEY)));
    const QStringList folded = ReadStrings(document.value(FOLDED_KEY));
    View_.Folded = QSet<QString>(folded.begin(), folded.end());
    const QJsonObject view = document.value(VIEW_KEY).toObject();
    View_.Query = view.value(QUERY_KEY).toString();
    View_.Tab = TabOf(view.value(TAB_KEY).toString());
    View_.ViewArchive = view.value(ARCHIVE_KEY).toBool(false);
    Compact_ = view.value(COMPACT_KEY).toBool(false);
    const QString opened = view.value(OPENED_KEY).toString();
    View_.Opened = IsSessionKey(opened) ? opened : QString();
    Unread_.remove(View_.Opened);

    Problem_.clear();
    const QJsonValue version = document.value(VERSION_KEY);
    const bool ours =
        version.isUndefined() ||
        (version.isDouble() &&
         version.toDouble() == static_cast<double>(DOCUMENT_VERSION));
    if (!ours) {
        const QByteArray text =
            QJsonDocument(QJsonArray{version}).toJson(QJsonDocument::Compact);
        Problem_ = QStringLiteral(
                       "sessions document has version %1, this roost reads "
                       "only version %2; the list is read-only")
                       .arg(QString::fromUtf8(text.mid(1, text.size() - 2)))
                       .arg(DOCUMENT_VERSION);
    }

    // A category of the document with no colour gets one.
    QStringList categories(Categories_.cbegin(), Categories_.cend());
    categories.sort();
    for (const QString& category : categories) {
        AssignColor(category);
    }

    if (!Listed_.empty()) {
        Reconcile(shownBefore);
    }
    const std::optional<QJsonObject> now = BuildDocument();
    return now.has_value() && *now != document;
}

std::optional<QJsonObject> ListState::ToDocument() const {
    return BuildDocument();
}

std::optional<QJsonObject> ListState::BuildDocument() const {
    if (ReadOnly()) {
        return std::nullopt;
    }
    QJsonObject colors;
    for (auto it = Colors_.begin(); it != Colors_.end(); ++it) {
        colors.insert(it.key(), it.value());
    }
    QJsonObject view;
    view.insert(QUERY_KEY, View_.Query);
    view.insert(TAB_KEY, TabName(View_.Tab));
    view.insert(ARCHIVE_KEY, View_.ViewArchive);
    view.insert(COMPACT_KEY, Compact_);
    view.insert(OPENED_KEY, View_.Opened);

    QJsonObject document;
    document.insert(VERSION_KEY, DOCUMENT_VERSION);
    document.insert(CATEGORIES_KEY, WriteStrings(Categories_));
    document.insert(NAMES_KEY, WriteStrings(Names_));
    document.insert(ARCHIVED_KEY, ToArray(Sorted(Archived_)));
    document.insert(COLORS_KEY, colors);
    document.insert(PINNED_KEY, ToArray(View_.Pinned));
    document.insert(FOLDED_KEY, ToArray(Sorted(View_.Folded)));
    document.insert(UNREAD_KEY, ToArray(Sorted(Unread_)));
    document.insert(STATES_KEY, WriteStrings(States_));
    document.insert(VIEW_KEY, view);
    return document;
}

bool ListState::Update(std::vector<SessionEntry> listed) {
    const QSet<QString> shownBefore = ShownUnread();
    Listed_ = std::move(listed);
    return Saved(Reconcile(shownBefore));
}

QSet<QString> ListState::ShownUnread() const {
    QSet<QString> out;
    for (const SessionEntry& session : Listed_) {
        const QString key = SessionKey(session.Agent, session.Id);
        if (IsShownUnread(key, session.State)) {
            out.insert(key);
        }
    }
    return out;
}

bool ListState::IsShownUnread(const QString& key, const QString& state) const {
    return Unread_.contains(key) && state != WORKING && key != View_.Opened;
}

bool ListState::Reconcile(const QSet<QString>& shownBefore) {
    bool changed = false;
    for (const SessionEntry& session : Listed_) {
        const QString key = SessionKey(session.Agent, session.Id);
        const auto known = States_.constFind(key);
        const bool moved = known == States_.cend() || *known != session.State;
        if (Seeded_ && moved && session.State != WORKING &&
            key != View_.Opened && !Unread_.contains(key)) {
            Unread_.insert(key);
            changed = true;
        }
        if (moved) {
            States_.insert(key, session.State);
            changed = true;
        }
        if (key == View_.Opened && Unread_.remove(key)) {
            changed = true;
        }
    }
    if (!Seeded_) {
        Seeded_ = true;
        changed = true;
    }
    RenewRanking(shownBefore);
    return changed;
}

void ListState::RenewRanking(const QSet<QString>& shownBefore) {
    const QSet<QString> current = ShownUnread();
    if (!shownBefore.contains(current)) {
        Ranked_ = current;
    }
}

std::vector<SessionEntry> ListState::Entries() const {
    std::vector<SessionEntry> out = Listed_;
    for (SessionEntry& entry : out) {
        const QString key = SessionKey(entry.Agent, entry.Id);
        entry.Name = Names_.value(key);
        entry.Category = Categories_.value(key);
        entry.Archived = Archived_.contains(key);
        entry.Unread = IsShownUnread(key, entry.State);
        entry.Ranked = Ranked_.contains(key);
    }
    return out;
}

int ListState::ColorIndex(const QString& category) const {
    return Colors_.value(category, -1);
}

void ListState::AssignColor(const QString& category) {
    if (category.isEmpty() || Colors_.contains(category)) {
        return;
    }
    int counts[COLOR_COUNT] = {};
    for (const int index : Colors_) {
        ++counts[index];
    }
    // The first colour not in use, else the least used (the first of them).
    const int* least = std::ranges::min_element(counts);
    Colors_.insert(category, static_cast<int>(least - counts));
}

bool ListState::SetCategory(const QString& key, const QString& category) {
    if (!IsSessionKey(key)) {
        return false;
    }
    const QString name = category.trimmed();
    AssignColor(name);
    if (name.isEmpty()) {
        return Saved(Categories_.remove(key) > 0);
    }
    const bool changed = Categories_.value(key) != name;
    Categories_.insert(key, name);
    return Saved(changed);
}

bool ListState::Rename(const QString& key, const QString& name) {
    if (!IsSessionKey(key)) {
        return false;
    }
    const QString given = name.trimmed();
    QString agentTitle;
    for (const SessionEntry& session : Listed_) {
        if (SessionKey(session.Agent, session.Id) == key) {
            agentTitle = session.AgentTitle;
            break;
        }
    }
    if (given.isEmpty() || given == agentTitle) {
        return Saved(Names_.remove(key) > 0);
    }
    const bool changed = Names_.value(key) != given;
    Names_.insert(key, given);
    return Saved(changed);
}

bool ListState::SetArchived(const QString& key, bool archived) {
    if (!IsSessionKey(key)) {
        return false;
    }
    if (!archived) {
        return Saved(Archived_.remove(key));
    }
    if (Archived_.contains(key)) {
        return false;
    }
    Archived_.insert(key);
    return Saved(true);
}

bool ListState::TogglePin(const QString& category) {
    if (View_.Pinned.removeAll(category) == 0) {
        View_.Pinned.append(category);
    }
    return Saved(true);
}

bool ListState::ToggleFold(const QString& category) {
    return Saved(Toggle(View_.Folded, category));
}

bool ListState::UnfoldAll() {
    const bool changed = !View_.Folded.isEmpty();
    View_.Folded.clear();
    return Saved(changed);
}

bool ListState::SetQuery(const QString& query) {
    return Saved(Assign(View_.Query, query));
}

bool ListState::SetTab(ETab tab) { return Saved(Assign(View_.Tab, tab)); }

bool ListState::SetViewArchive(bool archive) {
    return Saved(Assign(View_.ViewArchive, archive));
}

bool ListState::SetCompact(bool compact) {
    return Saved(Assign(Compact_, compact));
}

bool ListState::SetOpened(const QString& key) {
    if (!key.isEmpty() && !IsSessionKey(key)) {
        return false;
    }
    bool changed = Assign(View_.Opened, key);
    if (Unread_.remove(key)) {
        changed = true;
    }
    return Saved(changed);
}

bool ListState::Forget(const QString& key) {
    if (!IsSessionKey(key)) {
        return false;
    }
    bool changed = false;
    changed |= Categories_.remove(key) > 0;
    changed |= Names_.remove(key) > 0;
    changed |= Archived_.remove(key);
    changed |= Unread_.remove(key);
    changed |= States_.remove(key) > 0;
    Ranked_.remove(key);
    std::erase_if(Listed_, [&key](const SessionEntry& session) {
        return SessionKey(session.Agent, session.Id) == key;
    });
    if (View_.Opened == key) {
        View_.Opened.clear();
        changed = true;
    }
    return Saved(changed);
}

}  // namespace roost::sessions
