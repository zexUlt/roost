#include "qml_api/roost_singleton.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QtLogging>

#include "qml_api/js_callback.hpp"
#include "terminal/physical_keys.hpp"
#include "terminal/qt_key_mapping.hpp"

namespace roost {

RoostSingleton::RoostSingleton(QJSEngine& engine) : Engine_(engine) {}

RoostSingleton* RoostSingleton::create(QQmlEngine* /*qmlEngine*/,
                                       QJSEngine* jsEngine) {
    auto* const singleton = new RoostSingleton(*jsEngine);
    if (QCoreApplication* const app = QCoreApplication::instance()) {
        app->installEventFilter(singleton);
    }
    return singleton;
}

RoostSingleton::~RoostSingleton() {
    if (QCoreApplication* const app = QCoreApplication::instance()) {
        app->removeEventFilter(this);
    }
}

bool RoostSingleton::eventFilter(QObject* /*watched*/, QEvent* event) {
    if (event->type() == QEvent::KeyPress ||
        event->type() == QEvent::KeyRelease) {
        const auto& keyEvent = static_cast<const QKeyEvent&>(*event);
        Recorded_ = {.Key = keyEvent.key(),
                     .Modifiers = keyEvent.modifiers(),
                     .NativeScanCode = keyEvent.nativeScanCode(),
                     .NativeVirtualKey = keyEvent.nativeVirtualKey(),
                     .NativeModifiers = keyEvent.nativeModifiers(),
                     .Valid = true};
    }
    return false;
}

int RoostSingleton::bindingKey(QObject* event) const {
    if (event == nullptr) {
        return 0;
    }
    const int key = event->property("key").toInt();
    const auto modifiers =
        Qt::KeyboardModifiers::fromInt(event->property("modifiers").toUInt());
    EKey physical = EKey::Unidentified;
    if (Recorded_.Valid && Recorded_.Key == key &&
        Recorded_.Modifiers == modifiers) {
        const QKeyEvent recorded(
            QEvent::KeyPress, key, modifiers, Recorded_.NativeScanCode,
            Recorded_.NativeVirtualKey, Recorded_.NativeModifiers);
        const ENativeKeyCodes codes =
            QtKeyOptions::ForThisPlatform().NativeKeys;
        physical = PhysicalKey(codes, NativeKeyCode(recorded, codes));
    }
    return BindingKey(key, modifiers, physical);
}

void RoostSingleton::setApi(api::Api& api) {
    if (Api_ != nullptr) {
        qWarning("roost: Roost.setApi called twice; ignored");
        return;
    }
    Api_ = &api;
    Commands_ = new CommandsAdapter(api, Engine_, Owners_, this);
    Sessions_ = new SessionsModel(api, Engine_, Owners_, this);
    Store_ = new StoreAdapter(api, Engine_, Owners_, this);
    Bus_ = new BusAdapter(api, Engine_, Owners_, this);
    Connected_ = api.IsConnected();
    connect(&api, &api::Api::Connected, this, [this] { setConnected(true); });
    connect(&api, &api::Api::Disconnected, this,
            [this] { setConnected(false); });
}

void RoostSingleton::call(const QString& method, const QJSValue& params,
                          const QJSValue& callback) {
    QObject* const context = Owners_.contextFor(Engine_, this, method);
    if (context == nullptr) {
        return;
    }
    if (Api_ == nullptr) {
        answerLater(Engine_, context, callback, method,
                    std::unexpected(rpc::Error::FromCode(
                        rpc::EErrorCode::InternalError,
                        QStringLiteral("the API of the window is not set"))));
        return;
    }
    callMethod(*Api_, Engine_, context, method, toJson(Engine_, params),
               callback);
}

void RoostSingleton::setConnected(bool connected) {
    if (Connected_ != connected) {
        Connected_ = connected;
        emit connectedChanged();
    }
}

}  // namespace roost
