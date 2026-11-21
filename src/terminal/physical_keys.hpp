#pragma once

#include "core/vt_key_encoder.hpp"

#include <QKeyEvent>
#include <Qt>

#include <optional>
#include <span>

namespace roost {

// Which platform's native key codes an event carries: the physical key is
// read from them, since the Qt key of a non-Latin layout names the letter
// typed and not the key.
enum class ENativeKeyCodes {
    // No codes: no key is known.
    None,
    // macOS: QKeyEvent::nativeVirtualKey is NSEvent.keyCode, a kVK_ANSI_*
    // value (Events.h of HIToolbox). The key is positional; nativeScanCode is
    // always 0.
    MacVirtualKey,
    // Linux (xcb, wayland): QKeyEvent::nativeScanCode is the xkb keycode,
    // evdev code + 8 (linux/input-event-codes.h).
    XkbKeycode,
};

// The key of the US main block at a native code: a letter, a digit or one of
// the 11 symbol keys. Unidentified for any other code, for None and for no
// code (nullopt). On macOS code 0 is the A key, so "no code" must be nullopt,
// never 0.
EKey PhysicalKey(ENativeKeyCodes codes, std::optional<quint32> nativeKey);

// The native code the platform's events carry: nativeVirtualKey on macOS,
// nativeScanCode on xkb. nullopt for None and for an event that carries none:
// Qt's QKeyEvent constructor without native codes (synthetic events) zeroes
// scanCode, virtualKey and nativeModifiers; a real macOS key event carries
// nativeModifiers = NSEvent.modifierFlags, which is not 0 for a key event
// (the device-independent bit is set), and a real xkb keycode is never 0.
std::optional<quint32> NativeKeyCode(const QKeyEvent& event,
                                     ENativeKeyCodes codes);

// Whether the Qt key is a letter of a script other than Latin, which means
// the layout is not Latin. A letter of a Latin layout (Dvorak, QWERTZ, the
// French é), a letter of the Common or Inherited script (µ of macOS Option+m),
// a symbol and a special key are not.
bool IsNonLatinLetter(int qtKey);

// A key of the US main block that is not a letter: the 11 symbol keys and the
// 10 digits, with the Qt key each types without and with Shift (Qt names a
// shifted symbol by the symbol: Shift+1 is Qt::Key_Exclam).
struct SymbolKey {
    EKey Key;
    int Unshifted;
    int Shifted;
};

// The 21 symbol and digit keys, the one table of them: BindingKey and
// MapQtKey read it.
std::span<const SymbolKey> SymbolKeys();

// What the key of a press says about the layout, and what the last one that
// said anything was. A letter key of the US main block (A to Z) that types a
// letter tells the layout: a letter of another script than Latin makes the
// layout non-Latin, a Latin letter makes it Latin. Anything else (a symbol,
// a digit, a key of Dvorak that types a symbol at a letter position, a special
// key) says nothing. A Latin letter is believed only without Ctrl, Alt and
// Meta: with them some platforms give the Latin key of another group of the
// layout, which is no news about the layout. Call it for every key event
// before BindingKey; it is a state of the process, as the layout is.
void ObserveLayout(int qtKey, Qt::KeyboardModifiers modifiers, EKey physical);

// Whether the layout the last ObserveLayout judged is not Latin. Latin until a
// letter key has been seen.
bool NonLatinLayout();

// Sets the judgement without a key: for a test that must not depend on what
// an earlier one pressed.
void SetNonLatinLayout(bool nonLatin);

// The Qt key a binding matches: qtKey on a Latin layout, else the key the US
// layout types at the physical position. The layout is non-Latin when
// `nonLatinLayout` is set (NonLatinLayout()) or qtKey is itself a non-Latin
// letter. A letter is the capital (Qt::Key_J for both j and J); a digit or
// symbol key follows Shift, so Shift+Comma is Qt::Key_Less. qtKey stays when
// the key is unknown.
int BindingKey(int qtKey, Qt::KeyboardModifiers modifiers, EKey physical,
               bool nonLatinLayout = false);

}  // namespace roost
