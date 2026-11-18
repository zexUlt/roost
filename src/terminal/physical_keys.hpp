#pragma once

#include "core/vt_key_encoder.hpp"

#include <QKeyEvent>
#include <Qt>

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
// the 11 symbol keys. Unidentified for any other code and for None. On macOS
// code 0 is the A key, so an event without native codes reads as A there; on
// xkb 0 is no key.
EKey PhysicalKey(ENativeKeyCodes codes, quint32 nativeKey);

// The native code the platform's events carry: nativeVirtualKey on macOS,
// nativeScanCode on xkb, 0 for None.
quint32 NativeKeyCode(const QKeyEvent& event, ENativeKeyCodes codes);

// Whether the Qt key is a letter of a script other than Latin, which means
// the layout is not Latin. A letter of a Latin layout (Dvorak, QWERTZ, the
// French é), a symbol and a special key are not.
bool IsNonLatinLetter(int qtKey);

// The Qt key a binding matches: qtKey on a Latin layout, else the key the US
// layout types at the physical position. A letter is the capital (Qt::Key_J
// for both j and J); a digit or symbol key follows Shift, so Shift+Comma is
// Qt::Key_Less. qtKey stays when the key is unknown.
int BindingKey(int qtKey, Qt::KeyboardModifiers modifiers, EKey physical);

}  // namespace roost
