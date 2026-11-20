#pragma once

#include <QJsonObject>
#include <QString>

#include <array>
#include <expected>

#include "core/vt_render_state.hpp"

namespace roost {

// The colours of a terminal (design.md §5): what the screen shows by default
// and what a program that asks gets back (OSC 10, 11, 12, 4, the colour
// scheme report). The window and roostd both set them on their terminals, so
// that a program sees the colours the person sees.
struct TerminalColors {
    using Rgb = VtRenderState::Rgb;

    Rgb Foreground;
    Rgb Background;
    Rgb Cursor;
    // The background of selected text. libghostty-vt has no selection colour:
    // the window's renderer draws with it (src/terminal/renderer.cpp).
    Rgb Selection;
    // The 256-colour palette: 16 base colours, the 6x6x6 cube, 24 greys.
    std::array<Rgb, 256> Palette{};

    friend bool operator==(const TerminalColors&,
                           const TerminalColors&) = default;
};

// The relative luminance of a colour, 0 to 1, as WCAG defines it.
double Luminance(TerminalColors::Rgb color);

// The contrast ratio of two colours, 1 to 21, as WCAG defines it.
double Contrast(TerminalColors::Rgb first, TerminalColors::Rgb second);

// Whether the background is dark: what the colour scheme report (CSI ? 996 n)
// says. The point where black and white text read equally well on it.
bool IsDark(TerminalColors::Rgb background);

// The terminal part of a theme: the tokens terminalForeground,
// terminalBackground, terminalCursor and terminalSelection (each "#rrggbb"),
// terminalAnsi (an array of exactly 16 such colours: palette 0 to 15) and,
// optionally, terminalPalette (exactly 256: the whole palette, over which
// terminalAnsi is then put). Without terminalPalette, 16 to 255 are the
// xterm cube and greys, which is what libghostty-vt has. Keys of other
// tokens are ignored. The error names the first token that is missing or not
// a colour.
std::expected<TerminalColors, QString> ParseTerminalColors(
    const QJsonObject& theme);

}  // namespace roost
