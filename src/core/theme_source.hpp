#pragma once

#include <QJsonObject>
#include <QString>

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

}  // namespace roost
