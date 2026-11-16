#pragma once

#include "core/vt_key_encoder.hpp"
#include "core/vt_mouse_encoder.hpp"

#include <QString>
#include <Qt>

#include <optional>

namespace roost {

// What the Qt keyboard events of a platform mean, which the mapping cannot
// read from the event itself.
//
// macOS: Qt swaps the names of two modifiers. Qt::ControlModifier is the
// Command key and Qt::MetaModifier is the Control key (unless the
// application sets Qt::AA_MacDontSwapCtrlAndMeta, which roost does not), and
// likewise Qt::Key_Control and Qt::Key_Meta. Option is Qt::AltModifier, and
// Qt composes with it: Option+X has the text "≈", not "x". Arrow, Home and
// End keys carry Qt::KeypadModifier there, so that modifier says nothing
// about a key outside the keypad's own digits and operators.
// Elsewhere (Linux): Control is Control, Meta is the Super key, and Alt does
// not compose.
//
// Qt does not report the state of Caps Lock and Num Lock in the modifiers of
// a key event, nor which side of a modifier is down.
struct QtKeyOptions {
    // Qt's Control is the Command key and its Meta is the Control key.
    bool SwapControlAndMeta = false;
    // Option acts as Alt: the key is sent as ESC plus the character the key
    // has without Option, and the composed text is dropped. Only applies
    // with SwapControlAndMeta (macOS), where Option composes; on other
    // platforms Alt already acts as Alt.
    bool OptionAsAlt = false;

    // The conventions of the platform the program runs on; on macOS Option is
    // Alt, since a terminal has more use for Alt+B than for a composed "∫".
    static QtKeyOptions ForThisPlatform();
};

// The encoder's event for a Qt key event (Qt::Key, modifiers, text), or
// nothing when the event carries no key the encoder can use: a dead key or
// compose step that has no text yet, or a key outside the mapped set that
// types nothing.
//
// The text is the one Qt gives, except that control codes and macOS
// function-key codes (private use U+F700..U+F8FF) are dropped: the library
// wants those keys named by Key, not typed (see key/event.h). With Ctrl the
// text is rebuilt from the key as what it types without Ctrl ("_" for
// Ctrl+Shift+-, "[" for Ctrl+[), which is what the library's Ctrl rules read.
//
// A key outside the mapped set that does type text, such as a letter of a
// non-Latin layout, is Unidentified with that text, and is encoded by the text
// alone: so Ctrl with such a key sends nothing useful. Mapping by the native
// scan code would fix that; it is not done here.
//
// Known limit: a symbol key is mapped as the key of a US layout. With the
// Kitty "report all keys as escape codes" flag, a symbol typed through
// another layout (AltGr+Q gives "@" on a German one) is reported as the US
// key of that symbol. The fix is to map native scan codes to physical keys;
// that is a later change.
std::optional<KeyEvent> MapQtKey(int key, Qt::KeyboardModifiers modifiers,
                                 const QString& text, EKeyAction action,
                                 const QtKeyOptions& options);

// Whether the key is the platform's copy shortcut, as Ghostty binds it:
// Command+C on macOS (Qt's Control there), Ctrl+Shift+C elsewhere. Plain
// Ctrl+C stays the interrupt, and so does anything with another modifier
// besides these.
bool IsCopyShortcut(int key, Qt::KeyboardModifiers modifiers,
                    const QtKeyOptions& options);

// The modifiers of a mouse event, with the platform's names for Control and
// Meta as in MapQtKey. Shift, Control and Alt are what the mouse protocols
// carry; Super is set for completeness.
KeyMods MapQtModifiers(Qt::KeyboardModifiers modifiers,
                       const QtKeyOptions& options);

// The button an event is about; None for Qt::NoButton and for the buttons
// the protocols have no number for.
EMouseButton MapQtMouseButton(Qt::MouseButton button);

}  // namespace roost
