#include "sessions/qml_types/sessions_list.hpp"

#include <algorithm>
#include <utility>

#include "sessions/model/row_diff.hpp"

namespace roost::sessions {

namespace {

enum class ERole : int {
    Kind = Qt::UserRole,
    Key,
    Agent,
    Id,
    Title,
    AgentTitle,
    State,
    Cwd,
    StartedAt,
    Category,
    ColorIndex,
    Unread,
    IsOpen,
    Archived,
    Count,
    UnreadCount,
    Folded,
    Pinned,
};

constexpr int role(ERole value) { return static_cast<int>(value); }

const QString HEADER_PREFIX = QStringLiteral("header:");

QString keyOf(const QVariant& ref) {
    const QVariantMap map = ref.toMap();
    const QString agent = map.value(QStringLiteral("agent")).toString();
    const QString id = map.value(QStringLiteral("id")).toString();
    return agent.isEmpty() || id.isEmpty() ? QString() : SessionKey(agent, id);
}

QVariant nullVariant() { return QVariant::fromValue(nullptr); }

QString tabName(ETab tab) {
    switch (tab) {
        case ETab::Active:
            return QStringLiteral("active");
        case ETab::Unread:
            return QStringLiteral("unread");
        case ETab::All:
            break;
    }
    return QStringLiteral("all");
}

bool isStopRow(const Row& row) {
    return row.Kind == ERowKind::Session || row.Folded;
}

}  // namespace

struct SessionsList::Exposed {
    bool Ready = false;
    bool ReadOnly = false;
    QString Problem;
    QString Opened;
    QString Query;
    QString Tab;
    bool Archive = false;
    bool Compact = false;
    TabCounts Counts;
    QStringList Known;
    QString CursorKey;
    int CursorRow = -1;
    std::optional<QString> CursorCategory;
};

SessionsList::SessionsList(QObject* parent) : QAbstractListModel(parent) {}

QStringList SessionsList::sourceRoleNames() {
    return {QStringLiteral("agent"), QStringLiteral("id"),
            QStringLiteral("title"), QStringLiteral("cwd"),
            QStringLiteral("state"), QStringLiteral("startedAt")};
}

int SessionsList::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(Rows_.size());
}

QVariant SessionsList::data(const QModelIndex& index, int roleId) const {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(Rows_.size())) {
        return {};
    }
    const Row& row = Rows_[index.row()];
    const bool session = row.Kind == ERowKind::Session;
    switch (static_cast<ERole>(roleId)) {
        case ERole::Kind:
            return session ? QStringLiteral("session")
                           : QStringLiteral("header");
        case ERole::Key:
            return row.Key;
        case ERole::Category:
            return row.Category;
        case ERole::ColorIndex:
            return State_.ColorIndex(row.Category);
        case ERole::Agent:
            return session ? QVariant(row.Agent) : QVariant();
        case ERole::Id:
            return session ? QVariant(row.Id) : QVariant();
        case ERole::Title:
            return session ? QVariant(row.Title) : QVariant();
        case ERole::AgentTitle:
            return session ? QVariant(row.AgentTitle) : QVariant();
        case ERole::State:
            return session ? QVariant(row.State) : QVariant();
        case ERole::Cwd:
            return session ? QVariant(row.Cwd) : QVariant();
        case ERole::StartedAt:
            return session && row.StartedAt != 0 ? QVariant(row.StartedAt)
                                                 : QVariant();
        case ERole::Unread:
            return session ? QVariant(row.Unread) : QVariant();
        case ERole::IsOpen:
            return session ? QVariant(row.IsOpen) : QVariant();
        case ERole::Archived:
            return session ? QVariant(row.Archived) : QVariant();
        case ERole::Count:
            return session ? QVariant() : QVariant(row.Count);
        case ERole::UnreadCount:
            return session ? QVariant() : QVariant(row.UnreadCount);
        case ERole::Folded:
            return session ? QVariant() : QVariant(row.Folded);
        case ERole::Pinned:
            return session ? QVariant() : QVariant(row.Pinned);
    }
    return {};
}

QHash<int, QByteArray> SessionsList::roleNames() const {
    return {{role(ERole::Kind), "kind"},
            {role(ERole::Key), "key"},
            {role(ERole::Agent), "agent"},
            {role(ERole::Id), "id"},
            {role(ERole::Title), "title"},
            {role(ERole::AgentTitle), "agentTitle"},
            {role(ERole::State), "state"},
            {role(ERole::Cwd), "cwd"},
            {role(ERole::StartedAt), "startedAt"},
            {role(ERole::Category), "category"},
            {role(ERole::ColorIndex), "colorIndex"},
            {role(ERole::Unread), "unread"},
            {role(ERole::IsOpen), "isOpen"},
            {role(ERole::Archived), "archived"},
            {role(ERole::Count), "count"},
            {role(ERole::UnreadCount), "unreadCount"},
            {role(ERole::Folded), "folded"},
            {role(ERole::Pinned), "pinned"}};
}

// Properties.

QAbstractItemModel* SessionsList::source() const { return Source_; }

void SessionsList::setSource(QAbstractItemModel* source) {
    if (source == Source_) {
        return;
    }
    if (Source_ != nullptr) {
        disconnect(Source_, nullptr, this, nullptr);
    }
    Source_ = source;
    if (source != nullptr) {
        connect(source, &QAbstractItemModel::modelReset, this, [this] {
            ResetPending_ = true;
            scheduleSync();
        });
        connect(source, &QAbstractItemModel::destroyed, this, [this] {
            ResetPending_ = true;
            setSourceLoaded(false);
            scheduleSync();
        });
        connect(source, &QAbstractItemModel::rowsInserted, this,
                [this] { scheduleSync(); });
        connect(source, &QAbstractItemModel::rowsRemoved, this,
                [this] { scheduleSync(); });
        connect(source, &QAbstractItemModel::rowsMoved, this,
                [this] { scheduleSync(); });
        connect(source, &QAbstractItemModel::dataChanged, this,
                [this] { scheduleSync(); });
        connect(source, &QAbstractItemModel::layoutChanged, this,
                [this] { scheduleSync(); });
    }
    ResetPending_ = true;
    scheduleSync();
    emit sourceChanged();
}

bool SessionsList::sourceLoaded() const { return SourceLoaded_; }

void SessionsList::setSourceLoaded(bool loaded) {
    if (loaded == SourceLoaded_) {
        return;
    }
    SourceLoaded_ = loaded;
    scheduleSync();
    emit sourceLoadedChanged();
}

QVariant SessionsList::opened() const {
    const QString key = State_.View().Opened;
    const qsizetype slash = key.indexOf(QLatin1Char('/'));
    if (slash < 0) {
        return nullVariant();
    }
    return QVariantMap{{QStringLiteral("agent"), key.left(slash)},
                       {QStringLiteral("id"), key.mid(slash + 1)}};
}

void SessionsList::setOpened(const QVariant& ref) {
    if (!Ready_) {
        EarlyOpened_ = ref;
        return;
    }
    const QString key = keyOf(ref);
    edit([&] { return State_.SetOpened(key); });
}

bool SessionsList::ready() const { return Ready_; }

bool SessionsList::readOnly() const { return ReadFailed_ || State_.ReadOnly(); }

QString SessionsList::problem() const {
    if (!State_.Problem().isEmpty()) {
        return State_.Problem();
    }
    return !ReadProblem_.isEmpty() ? ReadProblem_ : SaveProblem_;
}

QString SessionsList::query() const { return State_.View().Query; }

void SessionsList::setQuery(const QString& query) {
    edit([&] { return State_.SetQuery(query); });
}

QString SessionsList::tab() const { return tabName(State_.View().Tab); }

void SessionsList::setTab(const QString& tab) {
    static const QStringList NAMES = {tabName(ETab::All), tabName(ETab::Active),
                                      tabName(ETab::Unread)};
    const qsizetype at = NAMES.indexOf(tab);
    if (at < 0) {
        return;
    }
    edit([&] { return State_.SetTab(static_cast<ETab>(at)); });
}

bool SessionsList::viewArchive() const { return State_.View().ViewArchive; }

void SessionsList::setViewArchive(bool archive) {
    edit([&] { return State_.SetViewArchive(archive); });
}

bool SessionsList::compact() const { return State_.Compact(); }

void SessionsList::setCompact(bool compact) {
    edit([&] { return State_.SetCompact(compact); });
}

QVariantMap SessionsList::counts() const {
    return {{QStringLiteral("all"), Counts_.All},
            {QStringLiteral("active"), Counts_.Active},
            {QStringLiteral("unread"), Counts_.Unread}};
}

QStringList SessionsList::knownCategories() const { return Known_; }

int SessionsList::cursorRow() const { return indexOfRow(CursorKey_); }

QVariant SessionsList::cursorSession() const {
    const int row = cursorRow();
    if (row < 0 || Rows_[row].Kind != ERowKind::Session) {
        return nullVariant();
    }
    return QVariantMap{{QStringLiteral("agent"), Rows_[row].Agent},
                       {QStringLiteral("id"), Rows_[row].Id}};
}

std::optional<QString> SessionsList::cursorCategoryOf() const {
    const int row = cursorRow();
    if (row < 0 || !isStopRow(Rows_[row])) {
        return std::nullopt;
    }
    return Rows_[row].Category;
}

QVariant SessionsList::cursorCategory() const {
    const std::optional<QString> category = cursorCategoryOf();
    return category ? QVariant(*category) : nullVariant();
}

// The cursor.

void SessionsList::move(int step) {
    const std::vector<QString> all = stops();
    if (all.empty()) {
        return;
    }
    const auto at = std::ranges::find(all, CursorKey_);
    int target = 0;
    if (at != all.end()) {
        target = std::clamp(static_cast<int>(at - all.begin()) + step, 0,
                            static_cast<int>(all.size()) - 1);
    }
    setCursor(all[target]);
}

bool SessionsList::setCursorRow(int row) {
    if (row < 0 || row >= static_cast<int>(Rows_.size()) ||
        !isStopRow(Rows_[row])) {
        return false;
    }
    setCursor(Rows_[row].Key);
    return true;
}

int SessionsList::rowOf(const QVariant& ref) const {
    const QString key = keyOf(ref);
    return key.isEmpty() ? -1 : indexOfRow(key);
}

QVariant SessionsList::refAt(int row) const {
    if (row < 0 || row >= static_cast<int>(Rows_.size()) ||
        Rows_[row].Kind != ERowKind::Session) {
        return nullVariant();
    }
    return QVariantMap{{QStringLiteral("agent"), Rows_[row].Agent},
                       {QStringLiteral("id"), Rows_[row].Id}};
}

// Edits.

void SessionsList::toggleFold(const QString& category) {
    edit([&] { return State_.ToggleFold(category); });
}

void SessionsList::unfoldAll() {
    edit([&] { return State_.UnfoldAll(); });
}

void SessionsList::togglePin(const QString& category) {
    edit([&] { return State_.TogglePin(category); });
}

void SessionsList::nextTab() {
    const ETab now = State_.View().Tab;
    const ETab next = now == ETab::All      ? ETab::Active
                      : now == ETab::Active ? ETab::Unread
                                            : ETab::All;
    edit([&] { return State_.SetTab(next); });
}

void SessionsList::setCategory(const QVariant& ref, const QString& name) {
    const QString key = keyOf(ref);
    edit([&] { return State_.SetCategory(key, name); });
}

void SessionsList::rename(const QVariant& ref, const QString& name) {
    const QString key = keyOf(ref);
    edit([&] { return State_.Rename(key, name); });
}

void SessionsList::setArchived(const QVariant& ref, bool archived) {
    const QString key = keyOf(ref);
    edit([&] { return State_.SetArchived(key, archived); });
}

void SessionsList::forget(const QVariant& ref) {
    const QString key = keyOf(ref);
    edit([&] { return State_.Forget(key); });
}

bool SessionsList::reveal(const QVariant& ref) {
    const QString key = keyOf(ref);
    const std::optional<QString> category = listedCategory(key);
    if (!Ready_ || !category) {
        return false;
    }
    bool archived = false;
    for (const SessionEntry& entry : State_.Entries()) {
        if (SessionKey(entry.Agent, entry.Id) == key) {
            archived = entry.Archived;
        }
    }
    edit([&] {
        bool changed = State_.SetQuery(QString());
        changed |= State_.SetViewArchive(archived);
        if (State_.View().Folded.contains(*category)) {
            changed |= State_.ToggleFold(*category);
        }
        return changed;
    });
    const bool shown = indexOfRow(key) >= 0;
    if (shown) {
        setCursor(key);
    }
    return shown;
}

// The store.

void SessionsList::restore(const QVariantMap& document) {
    const QJsonObject doc = QJsonObject::fromVariantMap(document);
    if (Ready_ && !ReadFailed_ && Echo_ && *Echo_ == doc) {
        Echo_.reset();
        return;
    }
    changing([&] {
        // A foreign document replaces the state, our unsent edits with it. A
        // write of ours still in flight is later in the store than this
        // document, so its echo is a foreign one too and is taken in.
        Echo_.reset();
        Dirty_ = false;
        bool save = State_.Restore(doc);
        ReadFailed_ = false;
        ReadProblem_.clear();
        Ready_ = true;
        if (EarlyOpened_) {
            save |= State_.SetOpened(keyOf(*EarlyOpened_));
            EarlyOpened_.reset();
        }
        syncBody();
        if (save) {
            requestSave();
        }
    });
}

void SessionsList::readFailed(const QString& error) {
    changing([&] {
        ReadFailed_ = true;
        ReadProblem_ =
            error.isEmpty() ? QStringLiteral("the store read failed") : error;
        Ready_ = true;
        if (EarlyOpened_) {
            State_.SetOpened(keyOf(*EarlyOpened_));
            EarlyOpened_.reset();
        }
        syncBody();
    });
}

void SessionsList::saveFinished(const QString& error) {
    if (!InFlight_) {
        return;
    }
    changing([&] {
        InFlight_ = false;
        Echo_.reset();
        if (!error.isEmpty()) {
            // The write did not happen: nothing will come back from it, and
            // the next edit or change of the source writes the state again.
            Dirty_ = true;
            SaveProblem_ = error;
            return;
        }
        SaveProblem_.clear();
        if (Dirty_) {
            sendSave();
        }
    });
}

void SessionsList::requestSave() {
    if (!Ready_ || readOnly()) {
        return;
    }
    Dirty_ = true;
    if (!InFlight_) {
        sendSave();
    }
}

void SessionsList::retrySave() {
    if (Dirty_ && !InFlight_ && Ready_ && !readOnly()) {
        sendSave();
    }
}

void SessionsList::sendSave() {
    const std::optional<QJsonObject> document = State_.ToDocument();
    if (!document || readOnly()) {
        Dirty_ = false;
        return;
    }
    InFlight_ = true;
    Dirty_ = false;
    Echo_ = *document;
    emit saveRequested(document->toVariantMap());
}

// Rows.

void SessionsList::scheduleSync() {
    if (SyncQueued_) {
        return;
    }
    SyncQueued_ = true;
    QMetaObject::invokeMethod(this, [this] { sync(); }, Qt::QueuedConnection);
}

void SessionsList::sync() {
    SyncQueued_ = false;
    if (Ready_) {
        changing([this] {
            syncBody();
            retrySave();
        });
    }
}

bool SessionsList::holding() const { return ResetPending_ && !SourceLoaded_; }

// Takes the source if it is loaded and publishes the rows. A reset source that
// is not loaded again keeps the state as it is and shows nothing: the cursor
// waits for the list.
void SessionsList::syncBody() {
    if (!Ready_) {
        return;
    }
    const bool hold = holding();
    if (SourceLoaded_ && State_.Update(readSource())) {
        requestSave();
    }
    const bool reset = ResetPending_ && SourceLoaded_;
    if (reset) {
        ResetPending_ = false;
    }
    publish(reset || (hold && !Rows_.empty()), hold);
}

void SessionsList::edit(const std::function<bool()>& apply) {
    if (!Ready_) {
        return;
    }
    changing([&] {
        const bool changed = apply();
        publish(false, holding());
        if (changed) {
            requestSave();
        }
    });
}

void SessionsList::publish(bool asReset, bool hold) {
    RowsResult result;
    if (!hold) {
        result = BuildRows(State_.Entries(), State_.View());
    }
    const std::vector<QString> oldStops = stops();
    if (asReset) {
        beginResetModel();
        Rows_ = std::move(result.Rows);
        endResetModel();
    } else {
        applySteps(result.Rows);
    }
    Counts_ = result.Counts;

    QSet<QString> known;
    if (!hold) {
        for (const SessionEntry& entry : State_.Entries()) {
            if (!entry.Category.isEmpty()) {
                known.insert(entry.Category);
            }
        }
    }
    Known_ = QStringList(known.begin(), known.end());
    Known_.sort();

    QHash<QString, int> colors;
    bool colorsMoved = false;
    for (const Row& row : Rows_) {
        const int index = State_.ColorIndex(row.Category);
        colors.insert(row.Category, index);
        const auto shown = ShownColors_.constFind(row.Category);
        colorsMoved |= shown != ShownColors_.cend() && *shown != index;
    }
    ShownColors_ = std::move(colors);
    if (colorsMoved && !asReset) {
        emit dataChanged(index(0), index(static_cast<int>(Rows_.size()) - 1),
                         {role(ERole::ColorIndex)});
    }

    if (!hold) {
        reconcileCursor(oldStops, asReset);
    }
}

void SessionsList::applySteps(const std::vector<Row>& rows) {
    for (const RowStep& step : DiffRows(Rows_, rows)) {
        switch (step.Kind) {
            case ERowStep::Remove:
                beginRemoveRows({}, step.From, step.From);
                Rows_.erase(Rows_.begin() + step.From);
                endRemoveRows();
                break;
            case ERowStep::Move:
                beginMoveRows({}, step.From, step.From, {}, step.To);
                std::rotate(Rows_.begin() + step.To, Rows_.begin() + step.From,
                            Rows_.begin() + step.From + 1);
                endMoveRows();
                break;
            case ERowStep::Insert:
                beginInsertRows({}, step.To, step.To);
                Rows_.insert(Rows_.begin() + step.To, rows[step.To]);
                endInsertRows();
                break;
            case ERowStep::Change:
                Rows_[step.To] = rows[step.To];
                emit dataChanged(index(step.To), index(step.To));
                break;
        }
    }
}

std::vector<QString> SessionsList::stops() const {
    std::vector<QString> out;
    for (const Row& row : Rows_) {
        if (isStopRow(row)) {
            out.push_back(row.Key);
        }
    }
    return out;
}

int SessionsList::indexOfRow(const QString& key) const {
    if (key.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < static_cast<int>(Rows_.size()); ++i) {
        if (Rows_[i].Key == key) {
            return i;
        }
    }
    return -1;
}

std::optional<QString> SessionsList::listedCategory(const QString& key) const {
    for (const SessionEntry& entry : State_.Entries()) {
        if (SessionKey(entry.Agent, entry.Id) == key) {
            return entry.Category;
        }
    }
    return std::nullopt;
}

bool SessionsList::categoryListed(const QString& category) const {
    const std::vector<SessionEntry> entries = State_.Entries();
    return std::ranges::any_of(entries, [&](const SessionEntry& entry) {
        return entry.Category == category;
    });
}

void SessionsList::setCursor(const QString& key) {
    if (key == CursorKey_) {
        return;
    }
    changing([&] { CursorKey_ = key; });
}

// The cursor after the rows changed. A kept stop stays. Otherwise: in a reset,
// the opened session, else the first stop; a header that was unfolded, its
// first session; a session folded away, its folded header; a session or
// header that is gone from the list, the stop that took its place (the
// previous if it was the last); anything else, the first stop.
void SessionsList::reconcileCursor(const std::vector<QString>& oldStops,
                                   bool reset) {
    const std::vector<QString> now = stops();
    const auto isStop = [&](const QString& key) {
        return std::ranges::find(now, key) != now.end();
    };
    if (now.empty()) {
        CursorKey_.clear();
        return;
    }
    if (!CursorKey_.isEmpty() && isStop(CursorKey_)) {
        return;
    }
    const QString first = now.front();
    if (reset) {
        const QString opened = State_.View().Opened;
        CursorKey_ = isStop(opened) ? opened : first;
        return;
    }
    if (CursorKey_.isEmpty()) {
        CursorKey_ = first;
        return;
    }
    const auto samePlace = [&] {
        const auto old = std::ranges::find(oldStops, CursorKey_);
        if (old == oldStops.end()) {
            return first;
        }
        const auto at = static_cast<std::size_t>(old - oldStops.begin());
        return now[std::min(at, now.size() - 1)];
    };
    if (CursorKey_.startsWith(HEADER_PREFIX)) {
        const int row = indexOfRow(CursorKey_);
        if (row >= 0) {
            const bool hasSession = row + 1 < static_cast<int>(Rows_.size()) &&
                                    Rows_[row + 1].Kind == ERowKind::Session;
            CursorKey_ = hasSession ? Rows_[row + 1].Key : first;
        } else if (!categoryListed(CursorKey_.mid(HEADER_PREFIX.size()))) {
            CursorKey_ = samePlace();
        } else {
            CursorKey_ = first;
        }
        return;
    }
    const std::optional<QString> category = listedCategory(CursorKey_);
    if (!category) {
        CursorKey_ = samePlace();
    } else if (isStop(HEADER_PREFIX + *category)) {
        CursorKey_ = HEADER_PREFIX + *category;
    } else {
        CursorKey_ = first;
    }
}

std::vector<SessionEntry> SessionsList::readSource() const {
    std::vector<SessionEntry> out;
    if (Source_ == nullptr) {
        return out;
    }
    QHash<QByteArray, int> roles;
    const QHash<int, QByteArray> names = Source_->roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it) {
        roles.insert(it.value(), it.key());
    }
    const auto read = [&](const QModelIndex& at, const char* name) {
        return Source_->data(at, roles.value(name, -1));
    };
    const int count = Source_->rowCount();
    for (int row = 0; row < count; ++row) {
        const QModelIndex at = Source_->index(row, 0);
        SessionEntry entry;
        entry.Agent = read(at, "agent").toString();
        entry.Id = read(at, "id").toString();
        if (entry.Agent.isEmpty() || entry.Id.isEmpty()) {
            continue;
        }
        entry.AgentTitle = read(at, "title").toString();
        entry.State = read(at, "state").toString();
        entry.Cwd = read(at, "cwd").toString();
        entry.StartedAt = read(at, "startedAt").toLongLong();
        out.push_back(std::move(entry));
    }
    return out;
}

// Announcing.

SessionsList::Exposed SessionsList::snapshot() const {
    Exposed out;
    out.Ready = Ready_;
    out.ReadOnly = readOnly();
    out.Problem = problem();
    out.Opened = State_.View().Opened;
    out.Query = State_.View().Query;
    out.Tab = tab();
    out.Archive = State_.View().ViewArchive;
    out.Compact = State_.Compact();
    out.Counts = Counts_;
    out.Known = Known_;
    out.CursorKey = CursorKey_;
    out.CursorRow = cursorRow();
    out.CursorCategory = cursorCategoryOf();
    return out;
}

void SessionsList::changing(const std::function<void()>& body) {
    const Exposed before = snapshot();
    body();
    announce(before);
}

void SessionsList::announce(const Exposed& before) {
    const Exposed now = snapshot();
    if (now.Ready != before.Ready) {
        emit readyChanged();
    }
    if (now.ReadOnly != before.ReadOnly) {
        emit readOnlyChanged();
    }
    if (now.Problem != before.Problem) {
        emit problemChanged();
    }
    if (now.Opened != before.Opened) {
        emit openedChanged();
    }
    if (now.Query != before.Query) {
        emit queryChanged();
    }
    if (now.Tab != before.Tab) {
        emit tabChanged();
    }
    if (now.Archive != before.Archive) {
        emit viewArchiveChanged();
    }
    if (now.Compact != before.Compact) {
        emit compactChanged();
    }
    if (!(now.Counts == before.Counts)) {
        emit countsChanged();
    }
    if (now.Known != before.Known) {
        emit knownCategoriesChanged();
    }
    if (now.CursorKey != before.CursorKey ||
        now.CursorRow != before.CursorRow ||
        now.CursorCategory != before.CursorCategory) {
        emit cursorChanged();
    }
}

}  // namespace roost::sessions
