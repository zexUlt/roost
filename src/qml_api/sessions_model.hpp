#pragma once

#include <QtQml/qqmlregistration.h>
#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QJSEngine>
#include <QJSValue>
#include <QModelIndex>
#include <QVariant>
#include <QVariantList>

#include "api/api.hpp"
#include "qml_api/calling_context.hpp"

namespace roost {

// Roost.sessions: the rows of api::SessionService as a list model. The roles
// are the fields of sessions.list, api::SessionService::FIELDS, in that order
// from Qt::UserRole; a field that a row lacks reads as undefined. The rows
// only grow between resets: nobody announces a session that is gone.
class SessionsModel : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS
    // The agents whose adapter did not answer with a list: [{agent, reason}].
    Q_PROPERTY(
        QVariantList unavailable READ unavailable NOTIFY unavailableChanged)
    // Whether the first list of the connection is in.
    Q_PROPERTY(bool loaded READ loaded NOTIFY loadedChanged)

public:
    SessionsModel(api::Api& api, QJSEngine& engine, const CallOwners& owners,
                  QObject* parent);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QVariantList unavailable() const;
    bool loaded() const;

    // Asks roostd for the list anew (sessions.list); the rows are replaced
    // when it answers. callback(error, result), as of every call of Roost.
    Q_INVOKABLE void refresh(const QJSValue& callback = QJSValue());

signals:
    void unavailableChanged();
    void loadedChanged();

private:
    api::Api& Api_;
    QJSEngine& Engine_;
    const CallOwners& Owners_;
    // SessionService changes its rows before it says so, so the count the
    // views were told is kept here.
    int Count_ = 0;
    // What was told last: the change signals go out only when these differ,
    // so a handler that refreshes on loadedChanged does not loop.
    bool Loaded_ = false;
    QVariantList Unavailable_;
};

}  // namespace roost
