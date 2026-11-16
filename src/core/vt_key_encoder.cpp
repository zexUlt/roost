#include "core/vt_key_encoder.hpp"

#include "core/ghostty_result.hpp"
#include "core/vt_terminal.hpp"

#include <ghostty/vt.h>

#include <array>
#include <utility>

namespace roost {

namespace {

// The value of `first` and the offset `key` has from `from`.
GhosttyKey Offset(GhosttyKey first, EKey key, EKey from) {
    return static_cast<GhosttyKey>(static_cast<int>(first) +
                                   static_cast<int>(key) -
                                   static_cast<int>(from));
}

GhosttyKey Translate(EKey key) {
    if (key >= EKey::Digit0 && key <= EKey::Digit9) {
        return Offset(GHOSTTY_KEY_DIGIT_0, key, EKey::Digit0);
    }
    if (key >= EKey::A && key <= EKey::Z) {
        return Offset(GHOSTTY_KEY_A, key, EKey::A);
    }
    if (key >= EKey::F1 && key <= EKey::F24) {
        return Offset(GHOSTTY_KEY_F1, key, EKey::F1);
    }
    if (key >= EKey::Keypad0 && key <= EKey::Keypad9) {
        return Offset(GHOSTTY_KEY_NUMPAD_0, key, EKey::Keypad0);
    }
    switch (key) {
        case EKey::Backquote:
            return GHOSTTY_KEY_BACKQUOTE;
        case EKey::Backslash:
            return GHOSTTY_KEY_BACKSLASH;
        case EKey::BracketLeft:
            return GHOSTTY_KEY_BRACKET_LEFT;
        case EKey::BracketRight:
            return GHOSTTY_KEY_BRACKET_RIGHT;
        case EKey::Comma:
            return GHOSTTY_KEY_COMMA;
        case EKey::Equal:
            return GHOSTTY_KEY_EQUAL;
        case EKey::Minus:
            return GHOSTTY_KEY_MINUS;
        case EKey::Period:
            return GHOSTTY_KEY_PERIOD;
        case EKey::Quote:
            return GHOSTTY_KEY_QUOTE;
        case EKey::Semicolon:
            return GHOSTTY_KEY_SEMICOLON;
        case EKey::Slash:
            return GHOSTTY_KEY_SLASH;
        case EKey::Space:
            return GHOSTTY_KEY_SPACE;
        case EKey::Enter:
            return GHOSTTY_KEY_ENTER;
        case EKey::Tab:
            return GHOSTTY_KEY_TAB;
        case EKey::Backspace:
            return GHOSTTY_KEY_BACKSPACE;
        case EKey::Escape:
            return GHOSTTY_KEY_ESCAPE;
        case EKey::Delete:
            return GHOSTTY_KEY_DELETE;
        case EKey::End:
            return GHOSTTY_KEY_END;
        case EKey::Home:
            return GHOSTTY_KEY_HOME;
        case EKey::Insert:
            return GHOSTTY_KEY_INSERT;
        case EKey::PageDown:
            return GHOSTTY_KEY_PAGE_DOWN;
        case EKey::PageUp:
            return GHOSTTY_KEY_PAGE_UP;
        case EKey::ArrowDown:
            return GHOSTTY_KEY_ARROW_DOWN;
        case EKey::ArrowLeft:
            return GHOSTTY_KEY_ARROW_LEFT;
        case EKey::ArrowRight:
            return GHOSTTY_KEY_ARROW_RIGHT;
        case EKey::ArrowUp:
            return GHOSTTY_KEY_ARROW_UP;
        case EKey::KeypadAdd:
            return GHOSTTY_KEY_NUMPAD_ADD;
        case EKey::KeypadSubtract:
            return GHOSTTY_KEY_NUMPAD_SUBTRACT;
        case EKey::KeypadMultiply:
            return GHOSTTY_KEY_NUMPAD_MULTIPLY;
        case EKey::KeypadDivide:
            return GHOSTTY_KEY_NUMPAD_DIVIDE;
        case EKey::KeypadDecimal:
            return GHOSTTY_KEY_NUMPAD_DECIMAL;
        case EKey::KeypadComma:
            return GHOSTTY_KEY_NUMPAD_COMMA;
        case EKey::KeypadEqual:
            return GHOSTTY_KEY_NUMPAD_EQUAL;
        case EKey::KeypadEnter:
            return GHOSTTY_KEY_NUMPAD_ENTER;
        case EKey::ShiftLeft:
            return GHOSTTY_KEY_SHIFT_LEFT;
        case EKey::ControlLeft:
            return GHOSTTY_KEY_CONTROL_LEFT;
        case EKey::AltLeft:
            return GHOSTTY_KEY_ALT_LEFT;
        case EKey::SuperLeft:
            return GHOSTTY_KEY_META_LEFT;
        case EKey::CapsLock:
            return GHOSTTY_KEY_CAPS_LOCK;
        case EKey::NumLock:
            return GHOSTTY_KEY_NUM_LOCK;
        default:
            return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

GhosttyKeyAction Translate(EKeyAction action) {
    switch (action) {
        case EKeyAction::Press:
            return GHOSTTY_KEY_ACTION_PRESS;
        case EKeyAction::Repeat:
            return GHOSTTY_KEY_ACTION_REPEAT;
        case EKeyAction::Release:
            return GHOSTTY_KEY_ACTION_RELEASE;
    }
    return GHOSTTY_KEY_ACTION_PRESS;
}

GhosttyMods Translate(const KeyMods& mods) {
    GhosttyMods bits = 0;
    if (mods.Shift) {
        bits |= GHOSTTY_MODS_SHIFT;
    }
    if (mods.Ctrl) {
        bits |= GHOSTTY_MODS_CTRL;
    }
    if (mods.Alt) {
        bits |= GHOSTTY_MODS_ALT;
    }
    if (mods.Super) {
        bits |= GHOSTTY_MODS_SUPER;
    }
    if (mods.CapsLock) {
        bits |= GHOSTTY_MODS_CAPS_LOCK;
    }
    if (mods.NumLock) {
        bits |= GHOSTTY_MODS_NUM_LOCK;
    }
    return bits;
}

}  // namespace

struct VtKeyEncoder::State {
    State(GhosttyKeyEncoder encoder, GhosttyKeyEvent event)
        : Encoder(encoder), Event(event) {}
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State() {
        ghostty_key_event_free(Event);
        ghostty_key_encoder_free(Encoder);
    }

    GhosttyKeyEncoder Encoder;
    GhosttyKeyEvent Event;
    bool OptionAsAlt = false;
    // The library does not copy the text of an event: it is read during
    // Encode, and this keeps it alive until then.
    QByteArray Text;
};

VtKeyEncoder::VtKeyEncoder(std::unique_ptr<State> state)
    : State_(std::move(state)) {}

VtKeyEncoder::VtKeyEncoder(VtKeyEncoder&&) noexcept = default;
VtKeyEncoder& VtKeyEncoder::operator=(VtKeyEncoder&&) noexcept = default;
VtKeyEncoder::~VtKeyEncoder() = default;

std::expected<VtKeyEncoder, QString> VtKeyEncoder::Create() {
    GhosttyKeyEncoder encoder = nullptr;
    if (const auto result = ghostty_key_encoder_new(nullptr, &encoder);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot create a key encoder", result));
    }
    GhosttyKeyEvent event = nullptr;
    if (const auto result = ghostty_key_event_new(nullptr, &event);
        result != GHOSTTY_SUCCESS) {
        ghostty_key_encoder_free(encoder);
        return std::unexpected(Failed("cannot create a key event", result));
    }
    return VtKeyEncoder(std::make_unique<State>(encoder, event));
}

void VtKeyEncoder::SetMacosOptionAsAlt(bool enabled) {
    State_->OptionAsAlt = enabled;
}

std::expected<void, QString> VtKeyEncoder::SyncFrom(
    const VtTerminal& terminal) {
    if (const auto result = ghostty_key_encoder_set_from_terminal(
            State_->Encoder,
            static_cast<GhosttyTerminal>(terminal.NativeHandle()));
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot sync the key encoder", result));
    }
    // The sync resets this option, since no terminal mode holds it.
    const GhosttyOptionAsAlt optionAsAlt = State_->OptionAsAlt
                                               ? GHOSTTY_OPTION_AS_ALT_TRUE
                                               : GHOSTTY_OPTION_AS_ALT_FALSE;
    if (const auto result = ghostty_key_encoder_set(
            State_->Encoder, GHOSTTY_KEY_ENCODER_OPT_MACOS_OPTION_AS_ALT,
            &optionAsAlt);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot sync the key encoder", result));
    }
    return {};
}

std::expected<QByteArray, QString> VtKeyEncoder::Encode(const KeyEvent& event) {
    State_->Text = event.Text.toUtf8();
    const GhosttyString text{
        .ptr = reinterpret_cast<const uint8_t*>(State_->Text.constData()),
        .len = static_cast<std::size_t>(State_->Text.size())};
    // These setters fail only for a null handle or an unknown enum value,
    // neither of which the translation produces.
    ghostty_key_event_set_action(State_->Event, Translate(event.Action));
    ghostty_key_event_set_key(State_->Event, Translate(event.Key));
    ghostty_key_event_set_mods(State_->Event, Translate(event.Mods));
    ghostty_key_event_set_consumed_mods(State_->Event,
                                        Translate(event.ConsumedMods));
    ghostty_key_event_set_utf8(State_->Event, text);
    ghostty_key_event_set_unshifted_codepoint(
        State_->Event, static_cast<uint32_t>(event.UnshiftedCodepoint));

    // Most sequences are a few bytes; the library says how many it needs
    // when they are not.
    std::array<uint8_t, 64> buffer{};
    std::size_t written = 0;
    auto result = ghostty_key_encoder_encode(
        State_->Encoder, State_->Event, buffer.data(), buffer.size(), &written);
    if (result == GHOSTTY_SUCCESS) {
        return QByteArray(reinterpret_cast<const char*>(buffer.data()),
                          static_cast<qsizetype>(written));
    }
    if (result != GHOSTTY_OUT_OF_SPACE) {
        return std::unexpected(Failed("cannot encode a key", result));
    }
    QByteArray bytes(static_cast<qsizetype>(written), Qt::Uninitialized);
    result = ghostty_key_encoder_encode(
        State_->Encoder, State_->Event,
        reinterpret_cast<uint8_t*>(bytes.data()),
        static_cast<std::size_t>(bytes.size()), &written);
    if (result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot encode a key", result));
    }
    bytes.truncate(static_cast<qsizetype>(written));
    return bytes;
}

}  // namespace roost
