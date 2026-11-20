#pragma once

#include <QtQml/qqmlregistration.h>
#include <QColor>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVariantList>

namespace roost {

// Roost.theme: the colours of the window and of the panels, by role (design.md
// §5). A role is what a colour is for, not where it is drawn: a QML file takes
// `Roost.theme.textSecondary`, never a literal, and a gate keeps it so
// (tools/check_qml_colors.sh). Built-in plugins read it through the public API
// like any other (ADR 009).
//
// The tokens are read-only to QML. The default theme is the resource
// themes/default.json; load() puts a theme over the current one (C++ only: a
// test, and the choice of a theme by the settings).
class Theme : public QObject {
    Q_OBJECT
    QML_ANONYMOUS

    // The window behind the panels.
    Q_PROPERTY(QColor windowBackground READ windowBackground NOTIFY changed)
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
    // The colours of the categories, eight, indexed by the model's colorIndex.
    Q_PROPERTY(QVariantList categories READ categories NOTIFY changed)
    // The connection banner: normal, refused, and its text.
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
    // follows if any token took a new value. Returns whether every key was
    // taken.
    bool load(const QJsonObject& tokens);

    QColor windowBackground() const { return WindowBackground_; }
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

private:
    QColor WindowBackground_;
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
    QVariantList Categories_;
    QColor BannerBackground_;
    QColor BannerRefusedBackground_;
    QColor BannerText_;
    QColor ProblemsBackground_;
    QColor ProblemsText_;
    QColor Warning_;
    QColor Error_;
    QColor WindowError_;
};

}  // namespace roost
