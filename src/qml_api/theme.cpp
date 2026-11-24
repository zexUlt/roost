#include "qml_api/theme.hpp"

#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonArray>
#include <QtLogging>

#include <algorithm>
#include <array>

#include "core/theme_source.hpp"
#include "terminal/renderer.hpp"

namespace roost {

namespace {

constexpr int CATEGORY_COUNT = 8;

struct Token {
    const char* Key;
    QColor Theme::* Member;
};

// uiFont at `ratio` of its size (the prototype's base is 13 px) and `weight`.
QFont scaled(const QFont& base, double ratio, QFont::Weight weight) {
    QFont font = base;
    if (base.pointSizeF() > 0) {
        font.setPointSizeF(base.pointSizeF() * ratio);
    } else if (base.pixelSize() > 0) {
        font.setPixelSize(
            std::max(1, static_cast<int>(base.pixelSize() * ratio + 0.5)));
    }
    font.setWeight(weight);
    return font;
}

}  // namespace

Theme::Theme(QObject* parent)
    : QObject(parent),
      DefaultTerminalFont_(DefaultTerminalFont()),
      DefaultUiFont_(QGuiApplication::font()),
      TerminalFont_(DefaultTerminalFont_),
      UiFont_(DefaultUiFont_) {
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
    followFonts(settings);
    return theme.Id;
}

void Theme::followFonts(const QJsonObject& settings) {
    const FontSettings wanted = ResolveFontSettings(settings);
    QStringList problems = wanted.Problems;

    QFont terminal = DefaultTerminalFont_;
    if (wanted.TerminalSize > 0) {
        terminal.setPointSizeF(wanted.TerminalSize);
    }
    if (!wanted.TerminalFamily.isEmpty()) {
        // A family that is not installed is not tried as a font: Qt would
        // answer with a substitute, which may well be a monospace one.
        QFont named(wanted.TerminalFamily);
        named.setPointSizeF(terminal.pointSizeF());
        // The generic name is the system monospace font itself, which
        // QFontDatabase does not list as a family.
        const bool generic =
            wanted.TerminalFamily.compare(QStringLiteral("monospace"),
                                          Qt::CaseInsensitive) == 0;
        if (generic) {
            // The default font, at the size computed above.
        } else if (QFontDatabase::hasFamily(wanted.TerminalFamily) &&
                   IsFixedPitch(named)) {
            terminal = named;
        } else {
            problems.append(
                QStringLiteral("settings.terminalFont.family \"%1\" is not "
                               "an installed monospace font; the system "
                               "monospace font is used")
                    .arg(wanted.TerminalFamily));
        }
    }
    QFont ui = DefaultUiFont_;
    if (wanted.UiSize > 0) {
        ui.setPointSizeF(wanted.UiSize);
    }

    const bool terminalDiffers = terminal != TerminalFont_;
    const bool uiDiffers = ui != UiFont_;
    const bool problemsDiffer = problems != FontProblems_;
    if (problemsDiffer) {
        for (const QString& problem : problems) {
            qWarning("roost: %s", qUtf8Printable(problem));
        }
    }
    TerminalFont_ = terminal;
    UiFont_ = ui;
    FontProblems_ = problems;
    if (terminalDiffers || uiDiffers || problemsDiffer) {
        emit changed();
    }
}

QFont Theme::titleFont() const {
    return scaled(UiFont_, 20.0 / 13.0, QFont::DemiBold);
}

QFont Theme::headingFont() const {
    return scaled(UiFont_, 16.0 / 13.0, QFont::DemiBold);
}

QFont Theme::rowFont() const {
    return scaled(UiFont_, 12.0 / 13.0, QFont::Medium);
}

QFont Theme::metaFont() const {
    return scaled(UiFont_, 11.0 / 13.0, UiFont_.weight());
}

QFont Theme::smallFont() const {
    return scaled(UiFont_, 10.5 / 13.0, UiFont_.weight());
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
        Token{"canvas", &Theme::Canvas_},
        Token{"surface", &Theme::Surface_},
        Token{"line", &Theme::Line_},
        Token{"selectedBackground", &Theme::SelectedBackground_},
        Token{"hoverBackground", &Theme::HoverBackground_},
        Token{"focus", &Theme::Focus_},
        Token{"attention", &Theme::Attention_},
        Token{"primary", &Theme::Primary_},
        Token{"primaryHover", &Theme::PrimaryHover_},
        Token{"primaryText", &Theme::PrimaryText_},
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
        const ResolvedTerminalColors resolved =
            ResolveTerminalColors(terminalTokens);
        if (resolved.Fallback) {
            qWarning("roost: theme: %s; the default terminal colours are used",
                     qUtf8Printable(resolved.Problem));
            taken = false;
            terminalTokens = ResolveTheme({}).Tokens;
        }
        if (!resolved.Colors) {
            qWarning("roost: theme: the default has no terminal colours: %s",
                     qUtf8Printable(resolved.Problem));
            taken = false;
        } else {
            TerminalTokens_ = terminalTokens;
            terminalDiffers = !(*resolved.Colors == TerminalColors_);
            TerminalColors_ = *resolved.Colors;
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
