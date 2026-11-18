#include "terminal/qt_key_mapping.hpp"

#include "terminal/physical_keys.hpp"

#include <QChar>
#include <QList>

#include <optional>

namespace roost {

namespace {

struct Mapped {
    EKey Key = EKey::Unidentified;
    // The key's character without Shift; 0 when the key has none.
    char32_t Unshifted = 0;
};

// The callers keep `by` inside the run of keys that `first` starts, which the
// analyzer cannot see.
EKey Advance(EKey first, int by) {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<EKey>(static_cast<int>(first) + by);
}

// Keys that type no character of their own.
EKey FunctionKey(int key, const QtKeyOptions& options) {
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        return Advance(EKey::F1, key - Qt::Key_F1);
    }
    switch (key) {
        case Qt::Key_Escape:
            return EKey::Escape;
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            return EKey::Tab;
        case Qt::Key_Backspace:
            return EKey::Backspace;
        case Qt::Key_Return:
            return EKey::Enter;
        case Qt::Key_Enter:
            return EKey::KeypadEnter;
        case Qt::Key_Insert:
            return EKey::Insert;
        case Qt::Key_Delete:
            return EKey::Delete;
        case Qt::Key_Home:
            return EKey::Home;
        case Qt::Key_End:
            return EKey::End;
        case Qt::Key_PageUp:
            return EKey::PageUp;
        case Qt::Key_PageDown:
            return EKey::PageDown;
        case Qt::Key_Left:
            return EKey::ArrowLeft;
        case Qt::Key_Right:
            return EKey::ArrowRight;
        case Qt::Key_Up:
            return EKey::ArrowUp;
        case Qt::Key_Down:
            return EKey::ArrowDown;
        case Qt::Key_Shift:
            return EKey::ShiftLeft;
        case Qt::Key_Control:
            return options.SwapControlAndMeta ? EKey::SuperLeft
                                              : EKey::ControlLeft;
        case Qt::Key_Meta:
            return options.SwapControlAndMeta ? EKey::ControlLeft
                                              : EKey::SuperLeft;
        case Qt::Key_Alt:
            return EKey::AltLeft;
        case Qt::Key_CapsLock:
            return EKey::CapsLock;
        case Qt::Key_NumLock:
            return EKey::NumLock;
        default:
            return EKey::Unidentified;
    }
}

// Digits and operators of the keypad, which Qt names like the main block's
// and tells apart by Qt::KeypadModifier alone.
EKey KeypadKey(int key) {
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return Advance(EKey::Keypad0, key - Qt::Key_0);
    }
    switch (key) {
        case Qt::Key_Plus:
            return EKey::KeypadAdd;
        case Qt::Key_Minus:
            return EKey::KeypadSubtract;
        case Qt::Key_Asterisk:
            return EKey::KeypadMultiply;
        case Qt::Key_Slash:
            return EKey::KeypadDivide;
        case Qt::Key_Period:
            return EKey::KeypadDecimal;
        case Qt::Key_Comma:
            return EKey::KeypadComma;
        case Qt::Key_Equal:
            return EKey::KeypadEqual;
        default:
            return EKey::Unidentified;
    }
}

Mapped MapKey(int key, bool keypad, const QtKeyOptions& options) {
    if (keypad) {
        if (const auto mapped = KeypadKey(key); mapped != EKey::Unidentified) {
            return {mapped, 0};
        }
    }
    if (const auto mapped = FunctionKey(key, options);
        mapped != EKey::Unidentified) {
        return {mapped, 0};
    }
    if (key == Qt::Key_Space) {
        return {EKey::Space, U' '};
    }
    // Qt names a letter key by its capital, whatever the layout, so a Latin
    // one is a physical key; a letter of another script falls through.
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return {Advance(EKey::A, key - Qt::Key_A),
                static_cast<char32_t>(U'a' +
                                      static_cast<char32_t>(key - Qt::Key_A))};
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return {Advance(EKey::Digit0, key - Qt::Key_0),
                static_cast<char32_t>(U'0' +
                                      static_cast<char32_t>(key - Qt::Key_0))};
    }
    // Qt names a shifted symbol by the symbol: Shift+1 is Qt::Key_Exclam. The
    // physical key and its unshifted character are those of a US layout; other
    // layouts get the key of the symbol they would type on a US one, which
    // only the Kitty protocol's reports can notice.
    for (const SymbolKey& symbol : SymbolKeys()) {
        if (key == symbol.Unshifted || key == symbol.Shifted) {
            return {symbol.Key, static_cast<char32_t>(symbol.Unshifted)};
        }
    }
    return {};
}

// A control code or a macOS function-key code is not text: the library
// wants the key named instead.
bool IsTyped(const QString& text) {
    if (text.isEmpty()) {
        return false;
    }
    const char16_t first = text.front().unicode();
    return first >= 0x20 && first != 0x7f &&
           !(first >= 0xf700 && first <= 0xf8ff);
}

// What a key types without Ctrl, for the keys Qt gives a control code instead
// of text under Ctrl. The library wants that character (Ctrl+Shift+- as "_"),
// not the code: it makes the code itself, or a CSI u sequence where the code
// would be ambiguous. Qt names a letter by its capital, a symbol by itself.
QString CtrlText(int key, bool shift) {
    if (key < 0x20 || key >= 0x7f) {
        return {};
    }
    const QChar typed(static_cast<char16_t>(key));
    return typed.isLetter() && !shift ? QString(typed.toLower())
                                      : QString(typed);
}

}  // namespace

QtKeyOptions QtKeyOptions::ForThisPlatform() {
#ifdef Q_OS_MACOS
    return {.SwapControlAndMeta = true,
            .OptionAsAlt = true,
            .NativeKeys = ENativeKeyCodes::MacVirtualKey};
#else
    return {.NativeKeys = ENativeKeyCodes::XkbKeycode};
#endif
}

std::optional<KeyEvent> MapQtKey(int key, Qt::KeyboardModifiers modifiers,
                                 const QString& text,
                                 std::optional<quint32> nativeKey,
                                 EKeyAction action,
                                 const QtKeyOptions& options) {
    const bool keypad = modifiers.testFlag(Qt::KeypadModifier);
    auto mapped = MapKey(key, keypad, options);
    if (mapped.Key == EKey::Unidentified && IsNonLatinLetter(key)) {
        // A letter of a non-Latin layout is the key at its position on the US
        // one; what the layout typed stays in the text.
        const EKey physical = PhysicalKey(options.NativeKeys, nativeKey);
        if (physical >= EKey::A && physical <= EKey::Z) {
            mapped = {physical, QChar::toLower(static_cast<char32_t>(key))};
        }
    }

    KeyEvent event;
    event.Action = action;
    event.Key = mapped.Key;
    event.UnshiftedCodepoint = mapped.Unshifted;
    event.Mods.Shift =
        modifiers.testFlag(Qt::ShiftModifier) || key == Qt::Key_Backtab;
    event.Mods.Alt = modifiers.testFlag(Qt::AltModifier);
    const bool control = modifiers.testFlag(Qt::ControlModifier);
    const bool meta = modifiers.testFlag(Qt::MetaModifier);
    event.Mods.Ctrl = options.SwapControlAndMeta ? meta : control;
    event.Mods.Super = options.SwapControlAndMeta ? control : meta;

    const bool optionAsAlt =
        options.SwapControlAndMeta && options.OptionAsAlt && event.Mods.Alt;
    if (optionAsAlt && key >= 0x20 && key < 0x7f) {
        // Qt names a Latin letter by its capital, a symbol by itself.
        const QChar typed(static_cast<char16_t>(key));
        event.Text =
            (typed.isLetter() && !event.Mods.Shift) ? typed.toLower() : typed;
    } else if (IsTyped(text)) {
        event.Text = text;
    } else if (event.Mods.Ctrl && event.Key != EKey::Unidentified) {
        // Qt's text is a control code or nothing.
        event.Text = CtrlText(key, event.Mods.Shift);
    }

    if (event.Key == EKey::Unidentified) {
        if (event.Text.isEmpty()) {
            return std::nullopt;
        }
        // A character of a layout the key table does not know.
        if (const auto typed = event.Text.toUcs4(); !typed.isEmpty()) {
            event.UnshiftedCodepoint = static_cast<char32_t>(
                QChar::toLower(static_cast<char32_t>(typed.front())));
        }
    }

    if (!event.Text.isEmpty()) {
        event.ConsumedMods.Shift = event.Mods.Shift;
        // Option composed the text: the layout used it up.
        event.ConsumedMods.Alt =
            options.SwapControlAndMeta && !optionAsAlt && event.Mods.Alt;
    }
    return event;
}

bool IsCopyShortcut(int key, Qt::KeyboardModifiers modifiers,
                    std::optional<quint32> nativeKey,
                    const QtKeyOptions& options) {
    const EKey physical = PhysicalKey(options.NativeKeys, nativeKey);
    if (BindingKey(key, modifiers, physical) != Qt::Key_C) {
        return false;
    }
    // The keypad flag rides on some keys on macOS and means nothing here.
    modifiers &= ~Qt::KeypadModifier;
    return options.SwapControlAndMeta
               ? modifiers == Qt::ControlModifier
               : modifiers == (Qt::ControlModifier | Qt::ShiftModifier);
}

KeyMods MapQtModifiers(Qt::KeyboardModifiers modifiers,
                       const QtKeyOptions& options) {
    KeyMods mods;
    mods.Shift = modifiers.testFlag(Qt::ShiftModifier);
    mods.Alt = modifiers.testFlag(Qt::AltModifier);
    const bool control = modifiers.testFlag(Qt::ControlModifier);
    const bool meta = modifiers.testFlag(Qt::MetaModifier);
    mods.Ctrl = options.SwapControlAndMeta ? meta : control;
    mods.Super = options.SwapControlAndMeta ? control : meta;
    return mods;
}

EMouseButton MapQtMouseButton(Qt::MouseButton button) {
    switch (button) {
        case Qt::LeftButton:
            return EMouseButton::Left;
        case Qt::MiddleButton:
            return EMouseButton::Middle;
        case Qt::RightButton:
            return EMouseButton::Right;
        case Qt::BackButton:
            return EMouseButton::Back;
        case Qt::ForwardButton:
            return EMouseButton::Forward;
        default:
            return EMouseButton::None;
    }
}

}  // namespace roost
