#include "qml_api/theme.hpp"

#include <QJsonArray>
#include <QtLogging>

#include <algorithm>
#include <array>

#include "core/theme_source.hpp"

namespace roost {

namespace {

constexpr int CATEGORY_COUNT = 8;

struct Token {
    const char* Key;
    QColor Theme::* Member;
};

}  // namespace

Theme::Theme(QObject* parent) : QObject(parent) {
    const ResolvedTheme theme = ResolveTheme({});
    if (!theme.Problem.isEmpty()) {
        qWarning("roost: %s", qUtf8Printable(theme.Problem));
    }
    if (!load(theme.Tokens)) {
        qWarning("roost: the default theme is not valid");
    }
}

QString Theme::follow(const QJsonObject& settings) {
    const ResolvedTheme theme = ResolveTheme(settings);
    if (!theme.Problem.isEmpty()) {
        qWarning("roost: %s", qUtf8Printable(theme.Problem));
    }
    load(theme.Tokens);
    return theme.Id;
}

bool Theme::load(const QJsonObject& tokens) {
    static const std::array TOKENS = {
        Token{"windowBackground", &Theme::WindowBackground_},
        Token{"windowText", &Theme::WindowText_},
        Token{"controlBackground", &Theme::ControlBackground_},
        Token{"inputBackground", &Theme::InputBackground_},
        Token{"panelBackground", &Theme::PanelBackground_},
        Token{"overlayBackground", &Theme::OverlayBackground_},
        Token{"text", &Theme::Text_},
        Token{"textStrong", &Theme::TextStrong_},
        Token{"textSecondary", &Theme::TextSecondary_},
        Token{"textMuted", &Theme::TextMuted_},
        Token{"accent", &Theme::Accent_},
        Token{"accentTint", &Theme::AccentTint_},
        Token{"selection", &Theme::Selection_},
        Token{"cursorFocused", &Theme::CursorFocused_},
        Token{"cursorUnfocused", &Theme::CursorUnfocused_},
        Token{"stateWorking", &Theme::StateWorking_},
        Token{"stateBlocked", &Theme::StateBlocked_},
        Token{"stateFailed", &Theme::StateFailed_},
        Token{"bannerBackground", &Theme::BannerBackground_},
        Token{"bannerRefusedBackground", &Theme::BannerRefusedBackground_},
        Token{"bannerText", &Theme::BannerText_},
        Token{"problemsBackground", &Theme::ProblemsBackground_},
        Token{"problemsText", &Theme::ProblemsText_},
        Token{"warning", &Theme::Warning_},
        Token{"error", &Theme::Error_},
        Token{"windowError", &Theme::WindowError_},
    };

    bool taken = true;
    bool differs = false;
    const auto colorOf = [](const QJsonValue& value) {
        return value.isString() ? QColor::fromString(value.toString())
                                : QColor();
    };

    QJsonObject terminalTokens = TerminalTokens_;
    bool terminalGiven = false;
    for (auto it = tokens.begin(); it != tokens.end(); ++it) {
        const QString key = it.key();
        if (key.startsWith(QLatin1String("terminal"))) {
            terminalTokens.insert(key, it.value());
            terminalGiven = true;
            continue;
        }
        if (key == QLatin1String("categories")) {
            QVariantList list;
            const QJsonArray array = it.value().toArray();
            for (const QJsonValue& value : array) {
                list.append(colorOf(value));
            }
            const bool valid = list.size() == CATEGORY_COUNT &&
                               std::ranges::all_of(list, [](const QVariant& v) {
                                   return v.value<QColor>().isValid();
                               });
            if (!valid) {
                qWarning("roost: theme: categories need %d colours; ignored",
                         CATEGORY_COUNT);
                taken = false;
            } else if (list != Categories_) {
                Categories_ = list;
                differs = true;
            }
            continue;
        }
        const auto* const token = std::ranges::find_if(
            TOKENS,
            [&key](const Token& t) { return key == QLatin1String(t.Key); });
        if (token == TOKENS.end()) {
            qWarning("roost: theme: unknown token %s; ignored",
                     qUtf8Printable(key));
            taken = false;
            continue;
        }
        const QColor color = colorOf(it.value());
        if (!color.isValid()) {
            qWarning("roost: theme: %s is not a colour; ignored",
                     qUtf8Printable(key));
            taken = false;
        } else if (this->*(token->Member) != color) {
            this->*(token->Member) = color;
            differs = true;
        }
    }
    bool terminalDiffers = false;
    if (terminalGiven) {
        const auto colors = ParseTerminalColors(terminalTokens);
        if (!colors) {
            qWarning("roost: theme: %s; the terminal tokens are ignored",
                     qUtf8Printable(colors.error()));
            taken = false;
        } else {
            TerminalTokens_ = terminalTokens;
            terminalDiffers = !(*colors == TerminalColors_);
            TerminalColors_ = *colors;
        }
    }
    if (differs || terminalDiffers) {
        emit changed();
    }
    if (terminalDiffers) {
        emit terminalColorsChanged();
    }
    return taken;
}

}  // namespace roost
