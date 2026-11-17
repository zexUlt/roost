#pragma once

#include <QtQml/qqmlregistration.h>
#include <QJSEngine>
#include <QJSValue>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include "api/api.hpp"
#include "qml_api/calling_context.hpp"

namespace roost {

// Roost.store: store.get and store.set of the dispatcher, and the changes of
// the documents that have been read.
class StoreAdapter : public QObject {
    Q_OBJECT
    QML_ANONYMOUS

public:
    StoreAdapter(api::Api& api, QJSEngine& engine, const CallOwners& owners,
                 QObject* parent);

    // callback(error, {document}).
    Q_INVOKABLE void get(const QString& plugin, const QJSValue& callback);
    // callback(error, null); the document is a JSON object, which roostd
    // checks. Whoever writes, `changed` follows.
    Q_INVOKABLE void set(const QString& plugin, const QJSValue& document,
                         const QJSValue& callback = QJSValue());

signals:
    // The document of a plugin that has been read was replaced, by this window
    // or another.
    void changed(const QString& plugin, const QJsonObject& document);

private:
    api::Api& Api_;
    QJSEngine& Engine_;
    const CallOwners& Owners_;
};

}  // namespace roost
