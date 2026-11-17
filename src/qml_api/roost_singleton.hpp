#pragma once

#include <QtQml/qqmlregistration.h>
#include <QJSEngine>
#include <QJSValue>
#include <QObject>
#include <QQmlEngine>
#include <QString>

#include "api/api.hpp"
#include "qml_api/bus_adapter.hpp"
#include "qml_api/commands_adapter.hpp"
#include "qml_api/sessions_model.hpp"
#include "qml_api/store_adapter.hpp"

namespace roost {

// The singleton Roost of module Roost.Api (design.md §5): the window's API as
// QML sees it. Thin adapters over api::Api: every method goes through its
// dispatcher, and an answer comes to a callback(error, result) in the GUI
// thread (error is null or {code, message, data?}).
//
// The engine owns the object. The shell gives it the Api with setApi before
// any QML that uses it is loaded; the Api must outlive the engine.
class RoostSingleton : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Roost)
    QML_SINGLETON
    Q_PROPERTY(roost::CommandsAdapter* commands READ commands CONSTANT)
    Q_PROPERTY(roost::SessionsModel* sessions READ sessions CONSTANT)
    Q_PROPERTY(roost::StoreAdapter* store READ store CONSTANT)
    Q_PROPERTY(roost::BusAdapter* bus READ bus CONSTANT)
    // Whether the window is connected to roostd now.
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)

public:
    static RoostSingleton* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    // From C++ only, once. Creates the parts that QML reaches by property.
    void setApi(api::Api& api);

    CommandsAdapter* commands() const { return Commands_; }
    SessionsModel* sessions() const { return Sessions_; }
    StoreAdapter* store() const { return Store_; }
    BusAdapter* bus() const { return Bus_; }
    bool connected() const { return Connected_; }

    // Any method of the dispatcher by name, as host->call of the C SDK does.
    // callback(error, result).
    Q_INVOKABLE void call(const QString& method,
                          const QJSValue& params = QJSValue(),
                          const QJSValue& callback = QJSValue());

signals:
    void connectedChanged();

private:
    explicit RoostSingleton(QJSEngine& engine);
    void setConnected(bool connected);

    QJSEngine& Engine_;
    api::Api* Api_ = nullptr;
    CommandsAdapter* Commands_ = nullptr;
    SessionsModel* Sessions_ = nullptr;
    StoreAdapter* Store_ = nullptr;
    BusAdapter* Bus_ = nullptr;
    bool Connected_ = false;
};

}  // namespace roost
