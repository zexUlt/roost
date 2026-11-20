#include "core/terminal_theme.hpp"

#include <QJsonArray>
#include <QJsonValue>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>

namespace roost {

namespace {

using Rgb = TerminalColors::Rgb;

constexpr int ANSI_COUNT = 16;
constexpr int PALETTE_COUNT = 256;
constexpr int CUBE_START = 16;
constexpr int GREY_START = 232;
// The channel values of the cube, as xterm has them.
constexpr std::array<quint8, 6> CUBE_LEVELS = {0, 95, 135, 175, 215, 255};

std::optional<Rgb> ParseColor(const QJsonValue& value) {
    if (!value.isString()) {
        return std::nullopt;
    }
    const QString text = value.toString();
    if (text.size() != 7 || text[0] != u'#') {
        return std::nullopt;
    }
    const QStringView digits = QStringView(text).mid(1);
    // toUInt would take a sign or a space; only hex digits are a colour.
    if (!std::ranges::all_of(digits, [](QChar c) {
            return c.unicode() < 128 && std::isxdigit(c.unicode()) != 0;
        })) {
        return std::nullopt;
    }
    const uint number = digits.toUInt(nullptr, 16);
    return Rgb{.R = static_cast<quint8>(number >> 16),
               .G = static_cast<quint8>(number >> 8),
               .B = static_cast<quint8>(number)};
}

std::expected<Rgb, QString> Color(const QJsonObject& theme, const char* key) {
    const auto color = ParseColor(theme.value(QLatin1String(key)));
    if (!color) {
        return std::unexpected(
            QStringLiteral("%1 is not a colour #rrggbb").arg(key));
    }
    return *color;
}

// The colours of the array into palette[0..]; the array must have exactly
// `count` of them.
std::expected<void, QString> ReadArray(const QJsonObject& theme,
                                       const char* key, int count,
                                       std::array<Rgb, 256>& palette) {
    const QJsonValue value = theme.value(QLatin1String(key));
    if (!value.isArray() || value.toArray().size() != count) {
        return std::unexpected(
            QStringLiteral("%1 needs %2 colours").arg(key).arg(count));
    }
    const QJsonArray array = value.toArray();
    for (int i = 0; i < count; ++i) {
        const auto color = ParseColor(array[i]);
        if (!color) {
            return std::unexpected(QStringLiteral("%1[%2] is not a colour "
                                                  "#rrggbb")
                                       .arg(key)
                                       .arg(i));
        }
        palette[static_cast<std::size_t>(i)] = *color;
    }
    return {};
}

double Linear(quint8 channel) {
    const double c = channel / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

}  // namespace

double Luminance(Rgb color) {
    return 0.2126 * Linear(color.R) + 0.7152 * Linear(color.G) +
           0.0722 * Linear(color.B);
}

double Contrast(Rgb first, Rgb second) {
    const double a = Luminance(first);
    const double b = Luminance(second);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

bool IsDark(Rgb background) {
    // Contrast with white is (1.05 / (L + 0.05)) and with black
    // ((L + 0.05) / 0.05): equal at L = 0.179.
    return Luminance(background) < 0.179;
}

std::expected<TerminalColors, QString> ParseTerminalColors(
    const QJsonObject& theme) {
    TerminalColors colors;
    const struct {
        const char* Key;
        Rgb TerminalColors::* Member;
    } scalars[] = {
        {"terminalForeground", &TerminalColors::Foreground},
        {"terminalBackground", &TerminalColors::Background},
        {"terminalCursor", &TerminalColors::Cursor},
        {"terminalSelection", &TerminalColors::Selection},
    };
    for (const auto& scalar : scalars) {
        const auto color = Color(theme, scalar.Key);
        if (!color) {
            return std::unexpected(color.error());
        }
        colors.*(scalar.Member) = *color;
    }

    for (int i = 0; i < 6 * 6 * 6; ++i) {
        colors.Palette[static_cast<std::size_t>(CUBE_START + i)] = {
            .R = CUBE_LEVELS[static_cast<std::size_t>(i / 36)],
            .G = CUBE_LEVELS[static_cast<std::size_t>(i / 6 % 6)],
            .B = CUBE_LEVELS[static_cast<std::size_t>(i % 6)]};
    }
    for (int i = 0; i < PALETTE_COUNT - GREY_START; ++i) {
        const auto grey = static_cast<quint8>(8 + 10 * i);
        colors.Palette[static_cast<std::size_t>(GREY_START + i)] = {
            .R = grey, .G = grey, .B = grey};
    }
    if (theme.contains(QLatin1String("terminalPalette"))) {
        if (auto read = ReadArray(theme, "terminalPalette", PALETTE_COUNT,
                                  colors.Palette);
            !read) {
            return std::unexpected(read.error());
        }
    }
    if (auto read =
            ReadArray(theme, "terminalAnsi", ANSI_COUNT, colors.Palette);
        !read) {
        return std::unexpected(read.error());
    }
    return colors;
}

}  // namespace roost
