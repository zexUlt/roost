#pragma once

#include <QJsonObject>
#include <QList>
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

// A setting of the `settings` document that cannot be used, and why: Setting is
// the key of the document (terminalFont, uiFontSize, focusColor), Text the line
// to show to the user. The window shows the line under the field of the
// setting and in the strip of problems.
struct SettingProblem {
    QString Setting;
    QString Text;

    bool operator==(const SettingProblem&) const = default;
};

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
    // One for each key that could not be used, to show to the user.
    QList<SettingProblem> Problems;
};

inline constexpr double MIN_FONT_SIZE = 6;
inline constexpr double MAX_FONT_SIZE = 72;

FontSettings ResolveFontSettings(const QJsonObject& settings);

// The colour of the focus ring that the settings ask for (design.md §5):
// `settings.focusColor` {preset, custom}. `preset` is "base" (the `focus` of
// the theme), "contrast" or "custom"; `custom` is "#rrggbb" and counts only
// for the preset "custom", where it is the colour. Without the key, or with
// the preset "base", Color stays empty: the theme's own `focus`. The same
// reader for every window. Any "#rrggbb" is taken; how readable it is on the
// cards is a warning of the GUI (Theme), not a refusal. Only what is not a
// colour is refused: a key that is not what it should be is a problem and
// leaves Color empty, the theme's own `focus`.
struct FocusSettings {
    // "#rrggbb" to take, or empty for the theme's own.
    QString Color;
    // One for each key that could not be used, to show to the user.
    QList<SettingProblem> Problems;
};

// The key of the focus colour in the settings document, and of its problems.
inline const QString FOCUS_COLOR_KEY = QStringLiteral("focusColor");
inline const QString FOCUS_CONTRAST_COLOR = QStringLiteral("#1b2430");

FocusSettings ResolveFocusSettings(const QJsonObject& settings);

}  // namespace roost
