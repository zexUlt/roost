#include "roost/key_chords.hpp"

#include <QKeyEvent>
#include <QQmlEngine>

#include "api/api.hpp"
#include "api/keymap_service.hpp"
#include "core/key_chord.hpp"
#include "qml_api/roost_singleton.hpp"
#include "roost/key_matcher.hpp"

namespace roost {

KeyChords::KeyChords(QObject* parent) : QObject(parent) {}

void KeyChords::componentComplete() {
    if (QQmlEngine* const engine = qmlEngine(this)) {
        Singleton_ =
            engine->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
    }
}

QString KeyChords::chordOf(QObject* event) const {
    if (event == nullptr || Singleton_ == nullptr) {
        return {};
    }
    // The key as the binding has it (by the layout and the position, which
    // the singleton knows from the native codes of the press), then named as
    // the matcher names a press.
    const int key = Singleton_->bindingKey(event);
    const auto modifiers =
        Qt::KeyboardModifiers::fromInt(event->property("modifiers").toUInt());
    const QKeyEvent press(QEvent::KeyPress, key, modifiers);
    const auto chord = KeyMatcher::ChordOf(press);
    return chord ? WriteKeyChord(*chord) : QString();
}

QString KeyChords::sameKey(const QString& text) const {
    const auto chord = ParseKeyChord(text, HostPlatform());
    return chord ? WriteKeyChord(api::MatchForm(*chord)) : QString();
}

QVariantMap KeyChords::holder(const QString& chordText,
                              const QString& when) const {
    if (Singleton_ == nullptr || Singleton_->api() == nullptr) {
        return {};
    }
    const auto chord = ParseKeyChord(chordText, HostPlatform());
    if (!chord) {
        return {};
    }
    const api::KeyBinding* const binding =
        Singleton_->api()->Keymap().Find(api::MatchForm(*chord), when);
    if (binding == nullptr) {
        return {};
    }
    return {{QStringLiteral("command"), binding->Command},
            {QStringLiteral("args"), binding->Args.toVariant()},
            {QStringLiteral("source"), binding->Source},
            {QStringLiteral("key"), WriteKeyChord(binding->Chord)}};
}

}  // namespace roost
