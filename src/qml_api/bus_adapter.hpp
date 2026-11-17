#pragma once

#include <QtQml/qqmlregistration.h>
#include <QJSEngine>
#include <QJSValue>
#include <QObject>
#include <QString>

#include "api/api.hpp"

namespace roost {

// Roost.bus: the event bus of roostd. Subscriptions are bound to an owner, so
// that a plugin's panel cannot leave a listener behind when it is destroyed.
class BusAdapter : public QObject {
    Q_OBJECT
    QML_ANONYMOUS

public:
    BusAdapter(api::Api& api, QJSEngine& engine, QObject* parent);

    // The id for unsubscribe. callback(name, payload) runs in the GUI thread
    // for each event of the pattern (a name, "prefix.*" or "*"), until
    // unsubscribe or until the owner is destroyed. Throws a TypeError for a
    // null owner, a callback that is not a function, or a bad pattern.
    Q_INVOKABLE qulonglong subscribe(const QString& pattern, QObject* owner,
                                     const QJSValue& callback);
    Q_INVOKABLE void unsubscribe(qulonglong id);
    // callback(error, null).
    Q_INVOKABLE void publish(const QString& name,
                             const QJSValue& payload = QJSValue(),
                             const QJSValue& callback = QJSValue());

private:
    api::Api& Api_;
    QJSEngine& Engine_;
};

}  // namespace roost
