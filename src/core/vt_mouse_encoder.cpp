#include "core/vt_mouse_encoder.hpp"

#include "core/ghostty_result.hpp"
#include "core/vt_terminal.hpp"

#include <ghostty/vt.h>

#include <array>
#include <utility>

namespace roost {

namespace {

GhosttyMouseAction Translate(EMouseAction action) {
    switch (action) {
        case EMouseAction::Press:
            return GHOSTTY_MOUSE_ACTION_PRESS;
        case EMouseAction::Release:
            return GHOSTTY_MOUSE_ACTION_RELEASE;
        case EMouseAction::Motion:
            return GHOSTTY_MOUSE_ACTION_MOTION;
    }
    return GHOSTTY_MOUSE_ACTION_PRESS;
}

GhosttyMouseButton Translate(EMouseButton button) {
    switch (button) {
        case EMouseButton::None:
            return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
        case EMouseButton::Left:
            return GHOSTTY_MOUSE_BUTTON_LEFT;
        case EMouseButton::Middle:
            return GHOSTTY_MOUSE_BUTTON_MIDDLE;
        case EMouseButton::Right:
            return GHOSTTY_MOUSE_BUTTON_RIGHT;
        case EMouseButton::WheelUp:
            return GHOSTTY_MOUSE_BUTTON_FOUR;
        case EMouseButton::WheelDown:
            return GHOSTTY_MOUSE_BUTTON_FIVE;
        case EMouseButton::WheelLeft:
            return GHOSTTY_MOUSE_BUTTON_SIX;
        case EMouseButton::WheelRight:
            return GHOSTTY_MOUSE_BUTTON_SEVEN;
        case EMouseButton::Back:
            return GHOSTTY_MOUSE_BUTTON_EIGHT;
        case EMouseButton::Forward:
            return GHOSTTY_MOUSE_BUTTON_NINE;
    }
    return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
}

bool IsWheel(EMouseButton button) {
    return button == EMouseButton::WheelUp ||
           button == EMouseButton::WheelDown ||
           button == EMouseButton::WheelLeft ||
           button == EMouseButton::WheelRight;
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
    return bits;
}

}  // namespace

struct VtMouseEncoder::State {
    State(GhosttyMouseEncoder encoder, GhosttyMouseEvent event)
        : Encoder(encoder), Event(event) {}
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State() {
        ghostty_mouse_event_free(Event);
        ghostty_mouse_encoder_free(Encoder);
    }

    GhosttyMouseEncoder Encoder;
    GhosttyMouseEvent Event;
    // What the last event reported, if it was a motion. The library can drop
    // repeated motions by itself, but SyncFrom resets that memory, and a
    // sync comes before every event; so the repeat is found here, by the
    // report being the same bytes. That holds for every format, SGR-pixels
    // included, where a move inside a cell is a different report.
    QByteArray LastMotion;
};

VtMouseEncoder::VtMouseEncoder(std::unique_ptr<State> state)
    : State_(std::move(state)) {}

VtMouseEncoder::VtMouseEncoder(VtMouseEncoder&&) noexcept = default;
VtMouseEncoder& VtMouseEncoder::operator=(VtMouseEncoder&&) noexcept = default;
VtMouseEncoder::~VtMouseEncoder() = default;

std::expected<VtMouseEncoder, QString> VtMouseEncoder::Create() {
    GhosttyMouseEncoder encoder = nullptr;
    if (const auto result = ghostty_mouse_encoder_new(nullptr, &encoder);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot create a mouse encoder", result));
    }
    GhosttyMouseEvent event = nullptr;
    if (const auto result = ghostty_mouse_event_new(nullptr, &event);
        result != GHOSTTY_SUCCESS) {
        ghostty_mouse_encoder_free(encoder);
        return std::unexpected(Failed("cannot create a mouse event", result));
    }
    return VtMouseEncoder(std::make_unique<State>(encoder, event));
}

std::expected<void, QString> VtMouseEncoder::SyncFrom(
    const VtTerminal& terminal, const MouseGeometry& geometry) {
    if (geometry.CellWidth == 0 || geometry.CellHeight == 0) {
        return std::unexpected(QStringLiteral("the mouse cell is empty"));
    }
    if (const auto result = ghostty_mouse_encoder_set_from_terminal(
            State_->Encoder,
            static_cast<GhosttyTerminal>(terminal.NativeHandle()));
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot sync the mouse encoder", result));
    }
    auto size = GHOSTTY_INIT_SIZED(GhosttyMouseEncoderSize);
    size.screen_width = geometry.ScreenWidth;
    size.screen_height = geometry.ScreenHeight;
    size.cell_width = geometry.CellWidth;
    size.cell_height = geometry.CellHeight;
    if (const auto result = ghostty_mouse_encoder_set(
            State_->Encoder, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot size the mouse encoder", result));
    }
    return {};
}

std::expected<QByteArray, QString> VtMouseEncoder::Encode(
    const MouseEvent& event) {
    // These setters fail only for a null handle or an unknown enum value,
    // neither of which the translation produces.
    ghostty_mouse_event_set_action(State_->Event, Translate(event.Action));
    if (event.Button == EMouseButton::None) {
        ghostty_mouse_event_clear_button(State_->Event);
    } else {
        ghostty_mouse_event_set_button(State_->Event, Translate(event.Button));
    }
    ghostty_mouse_event_set_mods(State_->Event, Translate(event.Mods));
    ghostty_mouse_event_set_position(State_->Event,
                                     {.x = event.X, .y = event.Y});
    const bool pressed = event.AnyButtonPressed;
    ghostty_mouse_encoder_set(State_->Encoder,
                              GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED,
                              &pressed);

    std::array<uint8_t, 64> buffer{};
    std::size_t written = 0;
    auto result = ghostty_mouse_encoder_encode(
        State_->Encoder, State_->Event, buffer.data(), buffer.size(), &written);
    QByteArray bytes;
    if (result == GHOSTTY_SUCCESS) {
        bytes = QByteArray(reinterpret_cast<const char*>(buffer.data()),
                           static_cast<qsizetype>(written));
    } else if (result == GHOSTTY_OUT_OF_SPACE) {
        bytes.resize(static_cast<qsizetype>(written));
        result = ghostty_mouse_encoder_encode(
            State_->Encoder, State_->Event,
            reinterpret_cast<uint8_t*>(bytes.data()),
            static_cast<std::size_t>(bytes.size()), &written);
        if (result != GHOSTTY_SUCCESS) {
            return std::unexpected(
                Failed("cannot encode a mouse event", result));
        }
        bytes.truncate(static_cast<qsizetype>(written));
    } else {
        return std::unexpected(Failed("cannot encode a mouse event", result));
    }

    if (event.Action != EMouseAction::Motion) {
        // A click makes the next motion news again; a wheel notch does not
        // move the pointer.
        if (!IsWheel(event.Button)) {
            State_->LastMotion.clear();
        }
    } else if (!bytes.isEmpty()) {
        if (bytes == State_->LastMotion) {
            return QByteArray();
        }
        State_->LastMotion = bytes;
    }
    return bytes;
}

}  // namespace roost
