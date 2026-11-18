#pragma once

#include "core/vt_key_encoder.hpp"
#include "core/vt_mouse_encoder.hpp"
#include "terminal/physical_keys.hpp"

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
    // Which platform's native key codes the events carry, to find the
    // physical key of a letter typed on a non-Latin layout. None: no key is
    // known, and such a letter is text only.
    ENativeKeyCodes NativeKeys = ENativeKeyCodes::None;

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
// A letter of a non-Latin layout (Qt names it by its own capital, U+041E for
// the Russian "о") is the key at its position on the US layout: with the
// native key code of the event and options.NativeKeys it is mapped to A..Z,
// so Ctrl+С sends the code of the C key. The text stays what the layout
// typed, and the unshifted codepoint is the lowercase of that letter. Without
// a code (nullopt, or NativeKeys None) such a letter is Unidentified with its
// text, and is encoded by the text alone.
//
// A key outside the mapped set that does type text is Unidentified with that
// text, and is encoded by the text alone.
//
// Known limit: a symbol key is mapped as the key of a US layout, and a symbol
// of a non-Latin layout is text only. With the Kitty "report all keys as
// escape codes" flag, a symbol typed through another layout (AltGr+Q gives
// "@" on a German one) is reported as the US key of that symbol.
std::optional<KeyEvent> MapQtKey(int key, Qt::KeyboardModifiers modifiers,
                                 const QString& text,
                                 std::optional<quint32> nativeKey,
                                 EKeyAction action,
                                 const QtKeyOptions& options);

// Whether the key is the platform's copy shortcut, as Ghostty binds it:
// Command+C on macOS (Qt's Control there), Ctrl+Shift+C elsewhere. Plain
// Ctrl+C stays the interrupt, and so does anything with another modifier
// besides these. The key is the one a binding matches (BindingKey), so
// Command+С copies on the Russian layout of macOS, and Ctrl+Shift+С on Linux.
bool IsCopyShortcut(int key, Qt::KeyboardModifiers modifiers,
                    std::optional<quint32> nativeKey,
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
