#include "core/theme_source.hpp"

#include <QFile>
#include <QJsonDocument>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>

namespace roost {

namespace {

constexpr auto DEFAULT_THEME_FILE = ":/roost/themes/default.json";
const QString TEST_THEME_ID = QStringLiteral("test");

std::optional<QJsonObject> ReadTokens(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}

// The size in `value` (named `key` in a problem), or 0 and a problem.
double ReadFontSize(const QJsonValue& value, const QString& key,
                    QStringList& problems) {
    const double size = value.toDouble(0);
    if (value.isDouble() && std::isfinite(size) && size >= MIN_FONT_SIZE &&
        size <= MAX_FONT_SIZE) {
        return size;
    }
    problems.append(
        QStringLiteral("%1 must be a number from %2 to %3; the default size "
                       "is used")
            .arg(key)
            .arg(MIN_FONT_SIZE)
            .arg(MAX_FONT_SIZE));
    return 0;
}

// Whether the text is "#rrggbb"; toUInt would take a sign or a space, so the
// digits are looked at one by one.
bool IsHexColor(const QString& text) {
    if (text.size() != 7 || text[0] != u'#') {
        return false;
    }
    return std::ranges::all_of(QStringView(text).mid(1), [](QChar c) {
        return c.unicode() < 128 && std::isxdigit(c.unicode()) != 0;
    });
}

}  // namespace

ResolvedTheme ResolveTheme(const QJsonObject& settings) {
    ResolvedTheme resolved{.Id = DEFAULT_THEME_ID, .Tokens = {}, .Problem = {}};
    const auto defaults = ReadTokens(QString::fromLatin1(DEFAULT_THEME_FILE));
    if (!defaults) {
        resolved.Problem =
            QStringLiteral("the default theme is missing or not valid");
        return resolved;
    }
    resolved.Tokens = *defaults;

    const QJsonValue wanted = settings.value(QStringLiteral("theme"));
    if (wanted.isUndefined() || wanted.toString() == DEFAULT_THEME_ID) {
        return resolved;
    }
    std::optional<QJsonObject> tokens;
    if (wanted.isString() && wanted.toString() == TEST_THEME_ID &&
        qEnvironmentVariableIsSet("ROOST_TEST_THEME_FILE")) {
        tokens = ReadTokens(qEnvironmentVariable("ROOST_TEST_THEME_FILE"));
    }
    if (!tokens) {
        resolved.Problem =
            wanted.isString()
                ? QStringLiteral("unknown theme \"%1\"; the default is used")
                      .arg(wanted.toString())
                : QStringLiteral(
                      "settings.theme is not a string; the default is used");
        return resolved;
    }
    for (auto it = tokens->begin(); it != tokens->end(); ++it) {
        resolved.Tokens.insert(it.key(), it.value());
    }
    resolved.Id = wanted.toString();
    return resolved;
}

ResolvedTerminalColors ResolveTerminalColors(const QJsonObject& tokens) {
    auto colors = ParseTerminalColors(tokens);
    if (colors) {
        return {.Colors = *colors, .Problem = {}, .Fallback = false};
    }
    const QString problem = colors.error();
    auto fallback = ParseTerminalColors(ResolveTheme({}).Tokens);
    if (!fallback) {
        return {.Colors = std::nullopt,
                .Problem = fallback.error(),
                .Fallback = false};
    }
    return {.Colors = *fallback, .Problem = problem, .Fallback = true};
}

FontSettings ResolveFontSettings(const QJsonObject& settings) {
    FontSettings fonts;
    const QJsonValue terminal = settings.value(QStringLiteral("terminalFont"));
    if (terminal.isObject()) {
        const QJsonObject object = terminal.toObject();
        const QJsonValue family = object.value(QStringLiteral("family"));
        if (family.isString() && !family.toString().trimmed().isEmpty()) {
            fonts.TerminalFamily = family.toString().trimmed();
        } else if (!family.isUndefined()) {
            fonts.Problems.append(
                QStringLiteral("settings.terminalFont.family must be a font "
                               "family name; the system monospace font is "
                               "used"));
        }
        const QJsonValue size = object.value(QStringLiteral("size"));
        if (!size.isUndefined()) {
            fonts.TerminalSize =
                ReadFontSize(size, QStringLiteral("settings.terminalFont.size"),
                             fonts.Problems);
        }
    } else if (!terminal.isUndefined()) {
        fonts.Problems.append(
            QStringLiteral("settings.terminalFont must be an object with "
                           "family and size; the defaults are used"));
    }
    const QJsonValue ui = settings.value(QStringLiteral("uiFontSize"));
    if (!ui.isUndefined()) {
        fonts.UiSize = ReadFontSize(ui, QStringLiteral("settings.uiFontSize"),
                                    fonts.Problems);
    }
    return fonts;
}

FocusSettings ResolveFocusSettings(const QJsonObject& settings) {
    FocusSettings focus;
    const QJsonValue wanted = settings.value(QStringLiteral("focusColor"));
    if (wanted.isUndefined()) {
        return focus;
    }
    if (!wanted.isObject()) {
        focus.Refused = true;
        focus.Problems.append(
            QStringLiteral("settings.focusColor must be an object with preset "
                           "and custom; the focus colour is kept"));
        return focus;
    }
    const QJsonObject object = wanted.toObject();
    const QJsonValue preset = object.value(QStringLiteral("preset"));
    if (preset.isUndefined() || preset.toString() == QLatin1String("base")) {
        return focus;
    }
    if (preset.toString() == QLatin1String("contrast")) {
        focus.Color = FOCUS_CONTRAST_COLOR;
        return focus;
    }
    if (preset.toString() == QLatin1String("custom")) {
        const QJsonValue custom = object.value(QStringLiteral("custom"));
        if (custom.isString() && IsHexColor(custom.toString())) {
            focus.Color = custom.toString().toLower();
            focus.Custom = true;
            return focus;
        }
        focus.Refused = true;
        focus.Problems.append(
            QStringLiteral("settings.focusColor.custom must be a colour "
                           "#rrggbb; the focus colour is kept"));
        return focus;
    }
    focus.Refused = true;
    focus.Problems.append(
        QStringLiteral("settings.focusColor.preset must be base, contrast or "
                       "custom; the focus colour is kept"));
    return focus;
}

}  // namespace roost
