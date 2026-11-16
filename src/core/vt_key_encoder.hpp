#pragma once

#include <QByteArray>
#include <QString>
#include <QtTypes>

#include <expected>
#include <memory>

namespace roost {

class VtTerminal;

// The physical key of a key event, by what the key is and not by what it
// types, as in the W3C UI Events "code" values libghostty-vt takes. The
// subset the window maps; a key outside it is Unidentified and is encoded by
// its text alone. The digit, letter, F and keypad digit runs are contiguous:
// the encoder's translation relies on it.
enum class EKey : quint16 {
    Unidentified,
    Backquote,
    Backslash,
    BracketLeft,
    BracketRight,
    Comma,
    Equal,
    Minus,
    Period,
    Quote,
    Semicolon,
    Slash,
    Space,
    Digit0,
    Digit1,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
    Digit7,
    Digit8,
    Digit9,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Enter,
    Tab,
    Backspace,
    Escape,
    Delete,
    End,
    Home,
    Insert,
    PageDown,
    PageUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
    ArrowUp,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    F13,
    F14,
    F15,
    F16,
    F17,
    F18,
    F19,
    F20,
    F21,
    F22,
    F23,
    F24,
    Keypad0,
    Keypad1,
    Keypad2,
    Keypad3,
    Keypad4,
    Keypad5,
    Keypad6,
    Keypad7,
    Keypad8,
    Keypad9,
    KeypadAdd,
    KeypadSubtract,
    KeypadMultiply,
    KeypadDivide,
    KeypadDecimal,
    KeypadComma,
    KeypadEqual,
    KeypadEnter,
    // Modifier keys alone: the protocols that report them need the event.
    ShiftLeft,
    ControlLeft,
    AltLeft,
    SuperLeft,
    CapsLock,
    NumLock,
};

enum class EKeyAction { Press, Repeat, Release };

// Which side of a modifier is down is not carried: Qt does not say.
struct KeyMods {
    bool Shift = false;
    bool Ctrl = false;
    bool Alt = false;
    // Command on macOS, the Windows key elsewhere.
    bool Super = false;
    bool CapsLock = false;
    bool NumLock = false;

    friend bool operator==(const KeyMods&, const KeyMods&) = default;
};

// One key event, as plain data: no Qt event and no library type in it.
struct KeyEvent {
    EKeyAction Action = EKeyAction::Press;
    EKey Key = EKey::Unidentified;
    KeyMods Mods;
    // The modifiers the keyboard layout used up in making Text (Shift for a
    // capital letter). The Kitty protocol leaves them out of the report.
    KeyMods ConsumedMods;
    // What the key types in the current layout before Ctrl or Alt change it:
    // a character, never a control code or a platform function-key code
    // (the library derives those from Key). Empty for keys that type
    // nothing.
    QString Text;
    // The character of the key without Shift, as a codepoint; 0 if unknown.
    // The Kitty protocol reports keys by it.
    char32_t UnshiftedCodepoint = 0;

    friend bool operator==(const KeyEvent&, const KeyEvent&) = default;
};

// Turns key events into the bytes a program expects, by libghostty-vt's key
// encoder (design.md §3). It is configured from a terminal's modes (cursor
// keys, keypad, Kitty keyboard protocol, ...), so SyncFrom has to run before
// the Encode of an event, and it is the only part that touches the terminal:
// Encode works on the encoder's own copy of the modes, so it needs no lock.
// One encoder is used from one thread at a time.
class VtKeyEncoder {
public:
    static std::expected<VtKeyEncoder, QString> Create();

    // A moved-from encoder may only be destroyed or assigned to.
    VtKeyEncoder(VtKeyEncoder&&) noexcept;
    VtKeyEncoder& operator=(VtKeyEncoder&&) noexcept;
    ~VtKeyEncoder();

    // Whether Option is Alt, as libghostty-vt's encoder has it when built for
    // macOS (elsewhere it has no effect). Not a terminal mode: the encoder
    // keeps the value across SyncFrom. Off by default, as in the library: an
    // Alt-modified key then encodes as the text the layout composed.
    void SetMacosOptionAsAlt(bool enabled);
    // Copies the terminal's current modes. The caller keeps the terminal
    // still until it returns.
    std::expected<void, QString> SyncFrom(const VtTerminal& terminal);
    // The bytes for one event; empty when it types nothing (a release in
    // legacy mode, a modifier alone).
    std::expected<QByteArray, QString> Encode(const KeyEvent& event);

private:
    struct State;

    explicit VtKeyEncoder(std::unique_ptr<State> state);

    std::unique_ptr<State> State_;
};

}  // namespace roost
