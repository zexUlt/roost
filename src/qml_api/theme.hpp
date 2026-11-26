#pragma once

#include <QtQml/qqmlregistration.h>
#include <QColor>
#include <QFont>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include "core/terminal_theme.hpp"
#include "core/theme_source.hpp"

namespace roost {

// Roost.theme: the colours of the window and of the panels, by role (design.md
// §5). A role is what a colour is for, not where it is drawn: a QML file takes
// `Roost.theme.textSecondary`, never a literal, and a gate keeps it so
// (tools/check_qml_colors.sh). Built-in plugins read it through the public API
// like any other (ADR 009).
//
// The tokens are read-only to QML. The default theme is the resource
// themes/default.json of the core library; load() puts a theme over the
// current one, and follow() takes the theme that a settings document names
// (both C++ only). The terminal tokens (terminalForeground, ...) are not
// properties: the terminals take them as terminalColors().
//
// The fonts are tokens too, but they come from the settings and not from a
// theme: terminalFont (the terminals' font) and uiFont (the font of the
// window), both `font` values. follow() reads settings.terminalFont
// {family, size} and settings.uiFontSize; what they leave out is the system
// monospace font at its size of today and the application's default font. The
// type scale (titleFont, ... smallFont) is uiFont at a ratio of its size.
//
// The `focus` token is the one colour that a setting may change: follow() takes
// settings.focusColor {preset, custom} over the theme's own `focus`. A custom
// colour is applied as it is; if it is below the contrast that 18-style.md asks
// of `focus` on `surface`, `canvas`, `cursorFocused` or `selectedBackground`
// (the worst pair of the current theme), settingsProblems has a line saying so.
// Only a key that is not a colour is refused, with a line too: the token is the
// theme's own.
class Theme : public QObject {
    Q_OBJECT
    QML_ANONYMOUS

    // The window behind the panels.
    Q_PROPERTY(QColor windowBackground READ windowBackground NOTIFY changed)
    // What Qt Quick Controls draw on the window: the text (also of buttons and
    // fields), the background of a button, the background of a text field.
    // They become the `palette` of the window (Main.qml).
    Q_PROPERTY(QColor windowText READ windowText NOTIFY changed)
    Q_PROPERTY(QColor controlBackground READ controlBackground NOTIFY changed)
    Q_PROPERTY(QColor inputBackground READ inputBackground NOTIFY changed)
    // The background of a panel, and of what lies over one.
    Q_PROPERTY(QColor panelBackground READ panelBackground NOTIFY changed)
    Q_PROPERTY(QColor overlayBackground READ overlayBackground NOTIFY changed)
    // Text on a panel: normal, emphasised, secondary, muted.
    Q_PROPERTY(QColor text READ text NOTIFY changed)
    Q_PROPERTY(QColor textStrong READ textStrong NOTIFY changed)
    Q_PROPERTY(QColor textSecondary READ textSecondary NOTIFY changed)
    Q_PROPERTY(QColor textMuted READ textMuted NOTIFY changed)
    // The accent, and its tint over a row; the selection of text.
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor accentTint READ accentTint NOTIFY changed)
    Q_PROPERTY(QColor selection READ selection NOTIFY changed)
    // The cursor of a list, with and without the keyboard focus.
    Q_PROPERTY(QColor cursorFocused READ cursorFocused NOTIFY changed)
    Q_PROPERTY(QColor cursorUnfocused READ cursorUnfocused NOTIFY changed)
    // The states of a session that stand out; the others take `text`.
    Q_PROPERTY(QColor stateWorking READ stateWorking NOTIFY changed)
    Q_PROPERTY(QColor stateBlocked READ stateBlocked NOTIFY changed)
    Q_PROPERTY(QColor stateFailed READ stateFailed NOTIFY changed)
    // The light style (docs/design/18-style.md). The canvas lies behind the
    // cards, the surface is a card, the line is the hairline around a card and
    // between the parts of one.
    Q_PROPERTY(QColor canvas READ canvas NOTIFY changed)
    Q_PROPERTY(QColor surface READ surface NOTIFY changed)
    Q_PROPERTY(QColor line READ line NOTIFY changed)
    // A selected row, a chosen filter, a hovered item of a list.
    Q_PROPERTY(QColor selectedBackground READ selectedBackground NOTIFY changed)
    Q_PROPERTY(QColor hoverBackground READ hoverBackground NOTIFY changed)
    // The focus ring, and the colour of what asks for attention (a waiting
    // session).
    Q_PROPERTY(QColor focus READ focus NOTIFY changed)
    // The two presets of settings.focusColor: the `focus` of the theme itself
    // (Base) and the fixed dark colour (Contrast), for the settings window to
    // show what each one is.
    Q_PROPERTY(QColor focusBase READ focusBase NOTIFY changed)
    Q_PROPERTY(QColor focusContrast READ focusContrast CONSTANT)
    Q_PROPERTY(QColor attention READ attention NOTIFY changed)
    // The primary button: normal, hovered, and its text.
    Q_PROPERTY(QColor primary READ primary NOTIFY changed)
    Q_PROPERTY(QColor primaryHover READ primaryHover NOTIFY changed)
    Q_PROPERTY(QColor primaryText READ primaryText NOTIFY changed)
    // Sizes in logical pixels: the radii, the spaces of the window and of a
    // list, the rings of the focus. Constants of the style, not of a theme.
    Q_PROPERTY(int radiusCard READ radiusCard CONSTANT)
    // How far a square background or a square of cells must stay from the edge
    // of a card, so that its corner lies inside the arc of radiusCard and the
    // line.
    Q_PROPERTY(int cornerInset READ cornerInset CONSTANT)
    Q_PROPERTY(int radiusControl READ radiusControl CONSTANT)
    Q_PROPERTY(int radiusSmall READ radiusSmall CONSTANT)
    Q_PROPERTY(int windowMargin READ windowMargin CONSTANT)
    Q_PROPERTY(int panelGap READ panelGap CONSTANT)
    Q_PROPERTY(int panelPadding READ panelPadding CONSTANT)
    Q_PROPERTY(int rowHeight READ rowHeight CONSTANT)
    Q_PROPERTY(int rowGap READ rowGap CONSTANT)
    Q_PROPERTY(int focusRingWidth READ focusRingWidth CONSTANT)
    Q_PROPERTY(int cursorRingWidth READ cursorRingWidth CONSTANT)
    // The type scale: uiFont at a ratio of its size (and at a weight), so it
    // follows settings.uiFontSize like uiFont. The title of a panel, the title
    // of the terminal, a row and a button, the meta line and the filters, the
    // small meta line.
    Q_PROPERTY(QFont titleFont READ titleFont NOTIFY changed)
    Q_PROPERTY(QFont headingFont READ headingFont NOTIFY changed)
    Q_PROPERTY(QFont rowFont READ rowFont NOTIFY changed)
    Q_PROPERTY(QFont metaFont READ metaFont NOTIFY changed)
    Q_PROPERTY(QFont smallFont READ smallFont NOTIFY changed)
    // The font of the terminals; the default is DefaultTerminalFont().
    Q_PROPERTY(QFont terminalFont READ terminalFont NOTIFY changed)
    // The font of the window: the application's default font, at the size of
    // settings.uiFontSize if there is one. The window (Main.qml) takes it as
    // its `font`, which the Controls and the Labels below it inherit; a plain
    // Text does not, so the panels use Label.
    Q_PROPERTY(QFont uiFont READ uiFont NOTIFY changed)
    // What is wrong with the settings that the theme reads (the fonts and
    // settings.focusColor): a list of {setting, text}, `setting` being the key
    // of the settings document (terminalFont, uiFontSize, focusColor) and
    // `text` the reason; empty when nothing is. The window shows the texts in
    // the strip of problems, and the settings window each under its field.
    Q_PROPERTY(
        QVariantList settingsProblems READ settingsProblems NOTIFY changed)
    // The colours of the categories, eight, indexed by the model's colorIndex.
    Q_PROPERTY(QVariantList categories READ categories NOTIFY changed)
    // The connection banner: normal, refused, and the text of the normal one
    // (a refusal and a failed start are in `error`).
    Q_PROPERTY(QColor bannerBackground READ bannerBackground NOTIFY changed)
    Q_PROPERTY(QColor bannerRefusedBackground READ bannerRefusedBackground
                   NOTIFY changed)
    Q_PROPERTY(QColor bannerText READ bannerText NOTIFY changed)
    // The strip of problems of the window.
    Q_PROPERTY(QColor problemsBackground READ problemsBackground NOTIFY changed)
    Q_PROPERTY(QColor problemsText READ problemsText NOTIFY changed)
    // A warning and an error on a panel; an error on the window.
    Q_PROPERTY(QColor warning READ warning NOTIFY changed)
    Q_PROPERTY(QColor error READ error NOTIFY changed)
    Q_PROPERTY(QColor windowError READ windowError NOTIFY changed)

public:
    // The default theme. Without its resource every token is invalid, and a
    // warning says so.
    explicit Theme(QObject* parent = nullptr);

    // Puts the tokens of the object over the current ones; a key the theme
    // does not know, and a value that is not a colour, are ignored with a
    // warning (`categories` takes an array of exactly eight colours). `changed`
    // follows if any token took a new value. The terminal tokens are checked
    // together, as ParseTerminalColors reads them: a bad one gives all the
    // terminal colours the default theme's (as roostd does);
    // terminalColorsChanged follows if the terminal colours changed. Returns
    // whether every key was taken.
    bool load(const QJsonObject& tokens);

    // The theme that the settings document names (settings.theme), all of its
    // tokens over the default ones; an unknown theme is the default one, with
    // a warning. Returns the id of the theme taken.
    QString follow(const QJsonObject& settings);

    // The fonts that the settings document names, over the default ones. A
    // family that is not installed or not monospace, and a size outside
    // MIN_FONT_SIZE..MAX_FONT_SIZE, are the defaults with an entry in
    // settingsProblems (and a warning). `changed` follows if a font or a
    // problem changed.
    void followFonts(const QJsonObject& settings);

    // The colours of the terminals, from the terminal tokens.
    const TerminalColors& terminalColors() const { return TerminalColors_; }

    QColor windowBackground() const { return WindowBackground_; }
    QColor windowText() const { return WindowText_; }
    QColor controlBackground() const { return ControlBackground_; }
    QColor inputBackground() const { return InputBackground_; }
    QColor panelBackground() const { return PanelBackground_; }
    QColor overlayBackground() const { return OverlayBackground_; }
    QColor text() const { return Text_; }
    QColor textStrong() const { return TextStrong_; }
    QColor textSecondary() const { return TextSecondary_; }
    QColor textMuted() const { return TextMuted_; }
    QColor accent() const { return Accent_; }
    QColor accentTint() const { return AccentTint_; }
    QColor selection() const { return Selection_; }
    QColor cursorFocused() const { return CursorFocused_; }
    QColor cursorUnfocused() const { return CursorUnfocused_; }
    QColor stateWorking() const { return StateWorking_; }
    QColor stateBlocked() const { return StateBlocked_; }
    QColor stateFailed() const { return StateFailed_; }
    QColor canvas() const { return Canvas_; }
    QColor surface() const { return Surface_; }
    QColor line() const { return Line_; }
    QColor selectedBackground() const { return SelectedBackground_; }
    QColor hoverBackground() const { return HoverBackground_; }
    QColor focus() const { return Focus_; }
    QColor focusBase() const { return FocusBase_; }
    QColor focusContrast() const;
    QColor attention() const { return Attention_; }
    QColor primary() const { return Primary_; }
    QColor primaryHover() const { return PrimaryHover_; }
    QColor primaryText() const { return PrimaryText_; }
    static constexpr int radiusCard() { return 12; }
    // The corner of the arc is radiusCard * (1 - 1/sqrt(2)) off each edge,
    // rounded up, and a pixel for the line.
    static constexpr int cornerInset() {
        constexpr double off = radiusCard() * (1.0 - 0.70710678118654752440);
        constexpr int whole = static_cast<int>(off);
        return whole + (off > whole ? 1 : 0) + 1;
    }
    static constexpr int radiusControl() { return 6; }
    static constexpr int radiusSmall() { return 5; }
    static constexpr int windowMargin() { return 16; }
    static constexpr int panelGap() { return 12; }
    static constexpr int panelPadding() { return 16; }
    static constexpr int rowHeight() { return 56; }
    static constexpr int rowGap() { return 3; }
    static constexpr int focusRingWidth() { return 3; }
    static constexpr int cursorRingWidth() { return 2; }
    // The font with another weight, and the digits of one width if asked; all
    // else in it, the size in points (fractions too) or in pixels among it, is
    // as it was. For an item that needs a theme font and a weight of its own:
    // `font:` cannot be combined with `font.weight:` on one item, and the
    // pieces of a font in QML cannot tell a size in pixels from one in points.
    Q_INVOKABLE static QFont withWeight(const QFont& font, int weight,
                                        bool tabularDigits = false);
    // The size of a font as a text for the settings: "13 pt" ("13.5 pt" for a
    // fraction, never "13.0 pt"), or "17 px" for a font sized in pixels.
    Q_INVOKABLE static QString sizeText(const QFont& font);
    QFont titleFont() const;
    QFont headingFont() const;
    QFont rowFont() const;
    QFont metaFont() const;
    QFont smallFont() const;
    QFont terminalFont() const { return TerminalFont_; }
    QFont uiFont() const { return UiFont_; }
    QVariantList settingsProblems() const;
    QVariantList categories() const { return Categories_; }
    QColor bannerBackground() const { return BannerBackground_; }
    QColor bannerRefusedBackground() const { return BannerRefusedBackground_; }
    QColor bannerText() const { return BannerText_; }
    QColor problemsBackground() const { return ProblemsBackground_; }
    QColor problemsText() const { return ProblemsText_; }
    QColor warning() const { return Warning_; }
    QColor error() const { return Error_; }
    QColor windowError() const { return WindowError_; }

signals:
    void changed();
    // The terminal colours are others now (changed follows).
    void terminalColorsChanged();

private:
    // Puts the focus colour of settings.focusColor into `tokens` (the theme's
    // tokens, which the colours of the FOCUS_PAIRS table are read from) and
    // keeps the problems of it, with the contrast warning of the worst pair.
    // Returns whether they are others than before.
    bool followFocus(const QJsonObject& settings, QJsonObject& tokens);
    // Takes `fresh` for the problems of the focus colour (or, if not `focus`,
    // of the fonts) and keeps the others as they are; the fonts come first.
    // Returns whether the list is another than before; a warning for each
    // entry of `fresh` if so.
    bool setProblems(const QList<SettingProblem>& fresh, bool focus);

    QColor WindowBackground_;
    QColor WindowText_;
    QColor ControlBackground_;
    QColor InputBackground_;
    QColor PanelBackground_;
    QColor OverlayBackground_;
    QColor Text_;
    QColor TextStrong_;
    QColor TextSecondary_;
    QColor TextMuted_;
    QColor Accent_;
    QColor AccentTint_;
    QColor Selection_;
    QColor CursorFocused_;
    QColor CursorUnfocused_;
    QColor StateWorking_;
    QColor StateBlocked_;
    QColor StateFailed_;
    QColor Canvas_;
    QColor Surface_;
    QColor Line_;
    QColor SelectedBackground_;
    QColor HoverBackground_;
    QColor Focus_;
    // The `focus` that the theme has, before settings.focusColor.
    QColor FocusBase_;
    QColor Attention_;
    QColor Primary_;
    QColor PrimaryHover_;
    QColor PrimaryText_;
    QVariantList Categories_;
    QColor BannerBackground_;
    QColor BannerRefusedBackground_;
    QColor BannerText_;
    QColor ProblemsBackground_;
    QColor ProblemsText_;
    QColor Warning_;
    QColor Error_;
    QColor WindowError_;
    // The terminal tokens as the themes gave them, laid over one another, and
    // what they parse to.
    QJsonObject TerminalTokens_;
    TerminalColors TerminalColors_;
    // The fonts of the settings; the defaults they start from are taken once.
    QFont DefaultTerminalFont_;
    QFont DefaultUiFont_;
    QFont TerminalFont_;
    QFont UiFont_;
    QList<SettingProblem> Problems_;
};

}  // namespace roost
