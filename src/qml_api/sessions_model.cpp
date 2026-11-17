#include "qml_api/sessions_model.hpp"

#include <QJsonObject>

#include "qml_api/js_callback.hpp"

namespace roost {

namespace {

constexpr int FIRST_ROLE = Qt::UserRole;

}  // namespace

SessionsModel::SessionsModel(api::Api& api, QJSEngine& engine, QObject* parent)
    : QAbstractListModel(parent),
      Api_(api),
      Engine_(engine),
      Count_(static_cast<int>(api.Sessions().Rows().size())) {
    const api::SessionService* sessions = &api.Sessions();
    connect(sessions, &api::SessionService::Reset, this, [this] {
        beginResetModel();
        Count_ = static_cast<int>(Api_.Sessions().Rows().size());
        endResetModel();
        emit unavailableChanged();
        emit loadedChanged();
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

QVariantList SessionsModel::unavailable() const {
    return Api_.Sessions().Unavailable().toVariantList();
}

bool SessionsModel::loaded() const { return Api_.Sessions().IsLoaded(); }

void SessionsModel::refresh(const QJSValue& callback) {
    callMethod(Api_, Engine_, this, QStringLiteral("sessions.list"),
               QJsonValue(QJsonValue::Undefined), callback);
}

}  // namespace roost
