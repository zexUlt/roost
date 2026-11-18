#include "terminal/physical_keys.hpp"

#include <QChar>

#include <array>
#include <optional>

namespace roost {

namespace {

struct NativeKey {
    EKey Key;
    // kVK_ANSI_* of Events.h.
    quint32 MacVirtualKey;
    // KEY_* of input-event-codes.h plus 8.
    quint32 XkbKeycode;
};

constexpr std::array<NativeKey, 47> NATIVE_KEYS = {{
    {EKey::Backquote, 0x32, 49},   {EKey::Backslash, 0x2A, 51},
    {EKey::BracketLeft, 0x21, 34}, {EKey::BracketRight, 0x1E, 35},
    {EKey::Comma, 0x2B, 59},       {EKey::Equal, 0x18, 21},
    {EKey::Minus, 0x1B, 20},       {EKey::Period, 0x2F, 60},
    {EKey::Quote, 0x27, 48},       {EKey::Semicolon, 0x29, 47},
    {EKey::Slash, 0x2C, 61},       {EKey::Digit0, 0x1D, 19},
    {EKey::Digit1, 0x12, 10},      {EKey::Digit2, 0x13, 11},
    {EKey::Digit3, 0x14, 12},      {EKey::Digit4, 0x15, 13},
    {EKey::Digit5, 0x17, 14},      {EKey::Digit6, 0x16, 15},
    {EKey::Digit7, 0x1A, 16},      {EKey::Digit8, 0x1C, 17},
    {EKey::Digit9, 0x19, 18},      {EKey::A, 0x00, 38},
    {EKey::B, 0x0B, 56},           {EKey::C, 0x08, 54},
    {EKey::D, 0x02, 40},           {EKey::E, 0x0E, 26},
    {EKey::F, 0x03, 41},           {EKey::G, 0x05, 42},
    {EKey::H, 0x04, 43},           {EKey::I, 0x22, 31},
    {EKey::J, 0x26, 44},           {EKey::K, 0x28, 45},
    {EKey::L, 0x25, 46},           {EKey::M, 0x2E, 58},
    {EKey::N, 0x2D, 57},           {EKey::O, 0x1F, 32},
    {EKey::P, 0x23, 33},           {EKey::Q, 0x0C, 24},
    {EKey::R, 0x0F, 27},           {EKey::S, 0x01, 39},
    {EKey::T, 0x11, 28},           {EKey::U, 0x20, 30},
    {EKey::V, 0x09, 55},           {EKey::W, 0x0D, 25},
    {EKey::X, 0x07, 53},           {EKey::Y, 0x10, 29},
    {EKey::Z, 0x06, 52},
}};

// The US layout's character of a key that is not a letter, by Qt key.
struct Symbol {
    EKey Key;
    int Unshifted;
    int Shifted;
};

constexpr std::array<Symbol, 21> SYMBOLS = {{
    {EKey::Backquote, Qt::Key_QuoteLeft, Qt::Key_AsciiTilde},
    {EKey::Backslash, Qt::Key_Backslash, Qt::Key_Bar},
    {EKey::BracketLeft, Qt::Key_BracketLeft, Qt::Key_BraceLeft},
    {EKey::BracketRight, Qt::Key_BracketRight, Qt::Key_BraceRight},
    {EKey::Comma, Qt::Key_Comma, Qt::Key_Less},
    {EKey::Equal, Qt::Key_Equal, Qt::Key_Plus},
    {EKey::Minus, Qt::Key_Minus, Qt::Key_Underscore},
    {EKey::Period, Qt::Key_Period, Qt::Key_Greater},
    {EKey::Quote, Qt::Key_Apostrophe, Qt::Key_QuoteDbl},
    {EKey::Semicolon, Qt::Key_Semicolon, Qt::Key_Colon},
    {EKey::Slash, Qt::Key_Slash, Qt::Key_Question},
    {EKey::Digit0, Qt::Key_0, Qt::Key_ParenRight},
    {EKey::Digit1, Qt::Key_1, Qt::Key_Exclam},
    {EKey::Digit2, Qt::Key_2, Qt::Key_At},
    {EKey::Digit3, Qt::Key_3, Qt::Key_NumberSign},
    {EKey::Digit4, Qt::Key_4, Qt::Key_Dollar},
    {EKey::Digit5, Qt::Key_5, Qt::Key_Percent},
    {EKey::Digit6, Qt::Key_6, Qt::Key_AsciiCircum},
    {EKey::Digit7, Qt::Key_7, Qt::Key_Ampersand},
    {EKey::Digit8, Qt::Key_8, Qt::Key_Asterisk},
    {EKey::Digit9, Qt::Key_9, Qt::Key_ParenLeft},
}};

}  // namespace

EKey PhysicalKey(ENativeKeyCodes codes, std::optional<quint32> nativeKey) {
    if (codes == ENativeKeyCodes::None || !nativeKey) {
        return EKey::Unidentified;
    }
    for (const NativeKey& row : NATIVE_KEYS) {
        const quint32 code = codes == ENativeKeyCodes::MacVirtualKey
                                 ? row.MacVirtualKey
                                 : row.XkbKeycode;
        if (code == *nativeKey) {
            return row.Key;
        }
    }
    return EKey::Unidentified;
}

std::optional<quint32> NativeKeyCode(const QKeyEvent& event,
                                     ENativeKeyCodes codes) {
    switch (codes) {
        case ENativeKeyCodes::MacVirtualKey:
            if (event.nativeVirtualKey() == 0 && event.nativeScanCode() == 0 &&
                event.nativeModifiers() == 0) {
                return std::nullopt;
            }
            return event.nativeVirtualKey();
        case ENativeKeyCodes::XkbKeycode:
            if (event.nativeScanCode() == 0) {
                return std::nullopt;
            }
            return event.nativeScanCode();
        case ENativeKeyCodes::None:
            break;
    }
    return std::nullopt;
}

bool IsNonLatinLetter(int qtKey) {
    // Qt names a printable key by its code point; the special keys start at
    // 0x01000000, above Unicode.
    if (qtKey <= 0 || qtKey > QChar::LastValidCodePoint) {
        return false;
    }
    const auto codePoint = static_cast<char32_t>(qtKey);
    if (!QChar::isLetter(codePoint)) {
        return false;
    }
    switch (QChar::script(codePoint)) {
        case QChar::Script_Latin:
        case QChar::Script_Common:
        case QChar::Script_Inherited:
        case QChar::Script_Unknown:
            return false;
        default:
            return true;
    }
}

int BindingKey(int qtKey, Qt::KeyboardModifiers modifiers, EKey physical) {
    if (!IsNonLatinLetter(qtKey)) {
        return qtKey;
    }
    if (physical >= EKey::A && physical <= EKey::Z) {
        return Qt::Key_A +
               (static_cast<int>(physical) - static_cast<int>(EKey::A));
    }
    for (const Symbol& symbol : SYMBOLS) {
        if (symbol.Key == physical) {
            return modifiers.testFlag(Qt::ShiftModifier) ? symbol.Shifted
                                                         : symbol.Unshifted;
        }
    }
    return qtKey;
}

}  // namespace roost
