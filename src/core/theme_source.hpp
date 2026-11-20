#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

#include "core/terminal_theme.hpp"

namespace roost {

// The theme the settings select (design.md §5): the id in `settings.theme`
// and the tokens of that theme. The window and roostd both resolve it, so
// that they cannot disagree.
//
// The only built-in theme is "default" (themes/default.json, a resource of the
// core library); it has every token. The tokens of another theme are laid over
// those of the default, so a theme names only what it changes.
struct ResolvedTheme {
    QString Id;
    QJsonObject Tokens;
    // Not empty when the settings named a theme that cannot be used and the
    // default was taken instead: the line to log.
    QString Problem;
};

inline const QString DEFAULT_THEME_ID = QStringLiteral("default");

// The theme of a `settings` document ({} for none). No `theme` key is the
// default theme without a problem; a `theme` that is not a string or names an
// unknown theme is the default one with a Problem.
//
// For tests only: when ROOST_TEST_THEME_FILE names a file with the tokens of a
// theme, that theme is known as "test", so that a test can switch roostd and
// the window to another theme without a second built-in one. Nothing sets it
// outside tests.
ResolvedTheme ResolveTheme(const QJsonObject& settings);

// The terminal colours of a theme's tokens, and the rule for a theme whose
// terminal tokens do not parse that the window and roostd share: the colours
// of the default theme (Fallback), Problem being what is wrong with the given
// tokens. Colors is empty only when the default theme itself does not parse
// (Problem says why); the caller keeps what it has.
struct ResolvedTerminalColors {
    std::optional<TerminalColors> Colors;
    QString Problem;
    bool Fallback = false;
};

ResolvedTerminalColors ResolveTerminalColors(const QJsonObject& tokens);

// The fonts the settings ask for (design.md §5): `settings.terminalFont`
// {family, size} and `settings.uiFontSize`. The same reader for every window;
// whether a family exists and is monospace is for the GUI to say (Theme), this
// only checks what the document says. A key that is absent is not a problem
// and stays empty (Family) or 0 (a size): the system monospace font, the size
// of today, the default UI font. A size is in points, from MIN_FONT_SIZE to
// MAX_FONT_SIZE; a size that is not a number or is outside them is 0 (the
// default) and a problem.
struct FontSettings {
    QString TerminalFamily;
    double TerminalSize = 0;
    double UiSize = 0;
    // One line for each key that could not be used, to show to the user.
    QStringList Problems;
};

inline constexpr double MIN_FONT_SIZE = 6;
inline constexpr double MAX_FONT_SIZE = 72;

FontSettings ResolveFontSettings(const QJsonObject& settings);

}  // namespace roost
