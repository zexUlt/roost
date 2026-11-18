#pragma once

#include <QtQml/qqmlregistration.h>
#include <QJSEngine>
#include <QJSValue>
#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QString>
#include <Qt>

#include "api/api.hpp"
#include "qml_api/bus_adapter.hpp"
#include "qml_api/calling_context.hpp"
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
    ~RoostSingleton() override;

    // From C++ only, once. Creates the parts that QML reaches by property.
    void setApi(api::Api& api);

    CommandsAdapter* commands() const { return Commands_; }
    SessionsModel* sessions() const { return Sessions_; }
    StoreAdapter* store() const { return Store_; }
    BusAdapter* bus() const { return Bus_; }
    bool connected() const { return Connected_; }

    // From C++ only. The window's API, or null until setApi (and in a window
    // that has none, such as the one of `roost --session`). The terminal item
    // reaches the IO thread through it.
    api::Api* api() const { return Api_; }

    // From C++ only. Returns the owner of the one-shot calls that QML code
    // makes from `context` (or a context below it). The owner belongs to the
    // context: when it is destroyed, their callbacks are not called, and a call
    // made after that is dropped. The loader may delete the owner first to
    // start unloading; deleting the context alone is also enough. A call from
    // a context with no registered one above it belongs to the singleton.
    // GUI thread.
    QObject* registerCallOwner(QQmlContext* context) {
        return Owners_.add(context);
    }

    // Any method of the dispatcher by name, as host->call of the C SDK does.
    // callback(error, result).
    Q_INVOKABLE void call(const QString& method,
                          const QJSValue& params = QJSValue(),
                          const QJSValue& callback = QJSValue());

    // The Qt key a binding matches for a QML KeyEvent: event.key on a Latin
    // layout, else the key the US layout types at the physical position of the
    // key (the letter j for the Russian "о", Qt.Key_Less for Shift+"б"). A
    // window-local helper, not a dispatcher method: it has no twin in
    // host->call. QQuickKeyEvent does not expose nativeVirtualKey, which macOS
    // needs, so the singleton records the native codes of the last key event
    // in an application event filter and uses them when the recorded key and
    // modifiers equal the KeyEvent's; otherwise event.key is returned. Call
    // it from a key handler, while the event is being delivered.
    Q_INVOKABLE int bindingKey(QObject* event) const;

signals:
    void connectedChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit RoostSingleton(QJSEngine& engine);
    void setConnected(bool connected);

    // The last key event the application filter saw.
    struct RecordedKey {
        int Key = 0;
        Qt::KeyboardModifiers Modifiers;
        quint32 NativeScanCode = 0;
        quint32 NativeVirtualKey = 0;
        quint32 NativeModifiers = 0;
        bool Valid = false;
    };

    QJSEngine& Engine_;
    RecordedKey Recorded_;
    CallOwners Owners_;
    api::Api* Api_ = nullptr;
    CommandsAdapter* Commands_ = nullptr;
    SessionsModel* Sessions_ = nullptr;
    StoreAdapter* Store_ = nullptr;
    BusAdapter* Bus_ = nullptr;
    bool Connected_ = false;
};

}  // namespace roost
