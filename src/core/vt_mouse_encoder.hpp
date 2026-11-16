#pragma once

#include "core/vt_key_encoder.hpp"

#include <QByteArray>
#include <QString>
#include <QtTypes>

#include <expected>
#include <memory>

namespace roost {

class VtTerminal;

enum class EMouseAction { Press, Release, Motion };

// The buttons by their numbers in the xterm protocol: 1-3 are the physical
// ones, 4-7 are the wheel, 8 and 9 are the side buttons. None is for motion
// with no button held.
enum class EMouseButton {
    None,
    Left,
    Middle,
    Right,
    WheelUp,
    WheelDown,
    WheelLeft,
    WheelRight,
    Back,
    Forward,
};

// One mouse event, as plain data: no Qt event and no library type in it.
struct MouseEvent {
    EMouseAction Action = EMouseAction::Press;
    EMouseButton Button = EMouseButton::None;
    // Only Shift, Ctrl and Alt matter to the protocols.
    KeyMods Mods;
    // Pixels from the top-left corner of the character grid; negative or
    // beyond the grid is allowed.
    float X = 0;
    float Y = 0;
    // Whether a button is held, this event's own button included. A motion
    // outside the grid is reported only while one is.
    bool AnyButtonPressed = false;
};

// The size of the grid on screen, in pixels, which turns a position into a
// cell. The cell must not be empty.
struct MouseGeometry {
    quint32 ScreenWidth = 0;
    quint32 ScreenHeight = 0;
    quint32 CellWidth = 1;
    quint32 CellHeight = 1;
};

// Turns mouse events into the bytes a program expects, by libghostty-vt's
// mouse encoder (design.md §3). Same shape as VtKeyEncoder: SyncFrom copies
// the terminal's tracking mode and report format, under whatever serializes
// access to the terminal, and Encode works on the encoder's own copy, so it
// needs no lock. One encoder is used from one thread at a time.
class VtMouseEncoder {
public:
    static std::expected<VtMouseEncoder, QString> Create();

    // A moved-from encoder may only be destroyed or assigned to.
    VtMouseEncoder(VtMouseEncoder&&) noexcept;
    VtMouseEncoder& operator=(VtMouseEncoder&&) noexcept;
    ~VtMouseEncoder();

    // Copies the terminal's tracking mode (X10, normal, button, any) and
    // format (X10, UTF-8, SGR, urxvt, SGR-pixels), and takes the geometry.
    // The caller keeps the terminal still until it returns.
    std::expected<void, QString> SyncFrom(const VtTerminal& terminal,
                                          const MouseGeometry& geometry);
    // The bytes for one event; empty when the terminal does not track the
    // mouse, when the mode leaves this event out (a release in X10 mode, a
    // motion in normal mode), when a legacy format cannot say the position
    // (a column or row past 223), and for a motion that reports the same as
    // the motion before it.
    std::expected<QByteArray, QString> Encode(const MouseEvent& event);

private:
    struct State;

    explicit VtMouseEncoder(std::unique_ptr<State> state);

    std::unique_ptr<State> State_;
};

}  // namespace roost
