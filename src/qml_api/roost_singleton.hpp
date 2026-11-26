#pragma once

#include <QtQml/qqmlregistration.h>
#include <QJSEngine>
#include <QJSValue>
#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <Qt>

#include "api/api.hpp"
#include "qml_api/bus_adapter.hpp"
#include "qml_api/calling_context.hpp"
#include "qml_api/commands_adapter.hpp"
#include "qml_api/sessions_model.hpp"
#include "qml_api/store_adapter.hpp"
#include "qml_api/theme.hpp"
#include "qml_api/updates.hpp"

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
    // The colours of the window by role (ADR 009: the built-in plugins read
    // them here too). Present before setApi.
    Q_PROPERTY(roost::Theme* theme READ theme CONSTANT)
    // Whether a newer release is out (design/17-updates.md). Present before
    // setApi, and idle without an API.
    Q_PROPERTY(roost::Updates* updates READ updates CONSTANT)
    // Whether the window is connected to roostd now.
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    // Whether the window has the keymap table (design.md §16). False before
    // the first answer of roostd, after a lost connection, and in a window
    // without an API: the keys of the table do not work then.
    Q_PROPERTY(bool keymapLoaded READ keymapLoaded NOTIFY keymapLoadedChanged)
    // The problems of the keymap table as roostd answered them (the texts of
    // design.md §16: keymap.json and the keys of the plugins). Empty while the
    // table is not loaded.
    Q_PROPERTY(QStringList keymapProblems READ keymapProblems NOTIFY
                   keymapProblemsChanged)

public:
    static RoostSingleton* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    ~RoostSingleton() override;

    // From C++ only, once. Creates the parts that QML reaches by property.
    void setApi(api::Api& api);

    CommandsAdapter* commands() const { return Commands_; }
    SessionsModel* sessions() const { return Sessions_; }
    StoreAdapter* store() const { return Store_; }
    BusAdapter* bus() const { return Bus_; }
    Theme* theme() const { return Theme_; }
    Updates* updates() const { return Updates_; }
    bool connected() const { return Connected_; }
    bool keymapLoaded() const { return KeymapLoaded_; }
    const QStringList& keymapProblems() const { return KeymapProblems_; }

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
    void bindCallOwnerPanel(const QObject* owner, QObject* panel) {
        Owners_.bindPanel(owner, panel);
    }

    // `plugin` is the id of the plugin whose panel the context is: the
    // methods that a plugin may call only for its own (keymap.setCommand,
    // keymap.resetCommand) are judged by it. Empty: no plugin.
    QObject* registerCallOwner(QQmlContext* context,
                               const QString& plugin = {}) {
        return Owners_.add(context, plugin);
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
    void keymapLoadedChanged();
    void keymapProblemsChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit RoostSingleton(QJSEngine& engine);
    void setConnected(bool connected);
    // Copies the problems of the keymap table; writes to the log each problem
    // of keymap.json that the previous list did not have (the list is empty
    // after a lost connection, so a reconnect writes them again).
    void UpdateKeymapProblems();

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
    Theme* Theme_ = nullptr;
    Updates* Updates_ = nullptr;
    bool Connected_ = false;
    bool KeymapLoaded_ = false;
    QStringList KeymapProblems_;
};

}  // namespace roost
