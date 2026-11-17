#include "qml_api/sessions_model.hpp"

#include <QJsonObject>

#include <utility>

#include "qml_api/js_callback.hpp"

namespace roost {

namespace {

constexpr int FIRST_ROLE = Qt::UserRole;

}  // namespace

SessionsModel::SessionsModel(api::Api& api, QJSEngine& engine,
                             const CallOwners& owners, QObject* parent)
    : QAbstractListModel(parent),
      Api_(api),
      Engine_(engine),
      Owners_(owners),
      Count_(static_cast<int>(api.Sessions().Rows().size())),
      Loaded_(api.Sessions().IsLoaded()),
      Unavailable_(api.Sessions().Unavailable().toVariantList()) {
    const api::SessionService* sessions = &api.Sessions();
    connect(sessions, &api::SessionService::Reset, this, [this] {
        beginResetModel();
        Count_ = static_cast<int>(Api_.Sessions().Rows().size());
        endResetModel();
        QVariantList unavailable =
            Api_.Sessions().Unavailable().toVariantList();
        if (unavailable != Unavailable_) {
            Unavailable_ = std::move(unavailable);
            emit unavailableChanged();
        }
        const bool loaded = Api_.Sessions().IsLoaded();
        if (loaded != Loaded_) {
            Loaded_ = loaded;
            emit loadedChanged();
        }
    });
    connect(sessions, &api::SessionService::RowInserted, this, [this](int row) {
        beginInsertRows({}, row, row);
        ++Count_;
        endInsertRows();
    });
    connect(sessions, &api::SessionService::RowChanged, this, [this](int row) {
        const QModelIndex changed = index(row);
        emit dataChanged(changed, changed);
    });
}

int SessionsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : Count_;
}

QVariant SessionsModel::data(const QModelIndex& index, int role) const {
    const auto& rows = Api_.Sessions().Rows();
    const int field = role - FIRST_ROLE;
    if (!index.isValid() || index.row() >= rows.size() || field < 0 ||
        field >= api::SessionService::FIELDS.size()) {
        return {};
    }
    return rows[index.row()]
        .value(api::SessionService::FIELDS[field])
        .toVariant();
}

QHash<int, QByteArray> SessionsModel::roleNames() const {
    QHash<int, QByteArray> names;
    for (int i = 0; i < api::SessionService::FIELDS.size(); ++i) {
        names.insert(FIRST_ROLE + i, api::SessionService::FIELDS[i].toUtf8());
    }
    return names;
}

QVariantList SessionsModel::unavailable() const { return Unavailable_; }

bool SessionsModel::loaded() const { return Loaded_; }

void SessionsModel::refresh(const QJSValue& callback) {
    QObject* const context =
        Owners_.contextFor(Engine_, this, QStringLiteral("sessions.list"));
    if (context == nullptr) {
        return;
    }
    callMethod(Api_, Engine_, context, QStringLiteral("sessions.list"),
               QJsonValue(QJsonValue::Undefined), callback);
}

}  // namespace roost
