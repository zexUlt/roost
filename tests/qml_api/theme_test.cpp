// Roost.theme: the tokens of the default theme are the colours the QML used to
// hard-code (so the look does not change), `load` puts a theme over them, and
// the elements of Main.qml and of the sessions panel take their colours from
// the tokens: a token that changes recolours them. The window, the daemon and
// the fake `claude` are helpers/sessions_window.hpp; the panels are the real
// ones.

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaProperty>
#include <QQmlExtensionPlugin>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <cmath>

#include "helpers/sessions_window.hpp"
#include "helpers/snapshot.hpp"
#include "qml_api/theme.hpp"

using namespace roost;
using namespace roost::testing;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

struct DefaultColor {
    const char* Token;
    const char* Value;
};

// Every colour token and its value in the default (light) theme, 18-style.md.
// A token that is not here fails EveryTokenIsInTheTable.
constexpr DefaultColor DEFAULT_COLORS[] = {
    // The window and the cards on it.
    {"windowBackground", "#f3f6f8"},
    {"canvas", "#f3f6f8"},
    {"surface", "#ffffff"},
    {"line", "#dde4ea"},
    {"panelBackground", "#ffffff"},
    {"overlayBackground", "#f2ffffff"},
    // The palette of the window for Qt Quick Controls.
    {"windowText", "#18232e"},
    {"controlBackground", "#f8fafc"},
    {"inputBackground", "#ffffff"},
    // Text.
    {"text", "#18232e"},
    {"textStrong", "#18232e"},
    {"textSecondary", "#5a6a7a"},
    {"textMuted", "#5f6e80"},
    // The accent, the rows, the focus.
    {"accent", "#225fc2"},
    {"accentTint", "#e9f1ff"},
    {"selection", "#cfe0fb"},
    {"cursorFocused", "#f8fbff"},
    {"cursorUnfocused", "#f2f5f8"},
    {"selectedBackground", "#e9f1ff"},
    {"hoverBackground", "#f5f8fb"},
    {"focus", "#225fc2"},
    {"attention", "#8b500b"},
    // The primary button.
    {"primary", "#2e66ce"},
    {"primaryHover", "#2658b5"},
    {"primaryText", "#ffffff"},
    // The states of a session.
    {"stateWorking", "#2b7a3b"},
    {"stateBlocked", "#8b500b"},
    {"stateFailed", "#c0262d"},
    // The banner and the strip of problems of the window.
    {"bannerBackground", "#fff4e0"},
    {"bannerRefusedBackground", "#fde8e8"},
    {"bannerText", "#8b500b"},
    {"problemsBackground", "#fff4e0"},
    {"problemsText", "#8b500b"},
    {"warning", "#8b500b"},
    {"error", "#c0262d"},
    {"windowError", "#b00020"},
};

// The colours of the categories (SessionsPanel.qml: categoryColors).
constexpr const char* DEFAULT_CATEGORIES[] = {"#0087d7", "#d70000", "#008700",
                                              "#d75f00", "#8700d7", "#008787",
                                              "#d700af", "#875f00"};

// The tokens that are colours, by the Q_PROPERTY of Theme; the sizes and the
// fonts (and what is wrong with them) are not colours.
QStringList TokenNames() {
    QStringList names;
    const QMetaObject& meta = Theme::staticMetaObject;
    for (int i = meta.propertyOffset(); i < meta.propertyCount(); ++i) {
        const QMetaProperty property = meta.property(i);
        if (property.metaType() != QMetaType::fromType<QColor>() &&
            property.metaType() != QMetaType::fromType<QVariantList>()) {
            continue;
        }
        // The list of {setting, text} is not a token either.
        if (QLatin1String(property.name()) ==
            QLatin1String("settingsProblems")) {
            continue;
        }
        names << QString::fromLatin1(property.name());
    }
    return names;
}

// The colour `index` of the test theme: none of them is a colour of the default
// theme, and each differs from the others.
QColor TestColor(int index) { return QColor::fromRgb(1, 2 + index, 100); }

// A theme of the test: every token, each its own colour.
QJsonObject TestTheme() {
    QJsonObject tokens;
    int index = 0;
    for (const DefaultColor& old : DEFAULT_COLORS) {
        tokens.insert(old.Token, TestColor(index++).name());
    }
    QJsonArray categories;
    for (int i = 0; i < 8; ++i) {
        categories.append(TestColor(100 + i).name());
    }
    tokens.insert("categories", categories);
    return tokens;
}

// The relative luminance and the contrast ratio of WCAG 2.x, for the pairs of
// the style guide (docs/design/18-style.md).
double Luminance(const QColor& color) {
    const auto channel = [](double value) {
        return value <= 0.04045 ? value / 12.92
                                : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(static_cast<double>(color.redF())) +
           0.7152 * channel(static_cast<double>(color.greenF())) +
           0.0722 * channel(static_cast<double>(color.blueF()));
}

double Contrast(const QColor& first, const QColor& second) {
    const double a = Luminance(first);
    const double b = Luminance(second);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

}  // namespace

class ThemeTest : public SessionsWindow {
    Q_OBJECT

private:
    static QColor ColorOf(const QQuickItem* item) {
        return item->property("color").value<QColor>();
    }

    // The colour the test theme gave to the token.
    static QColor Given(const char* token) {
        int index = 0;
        for (const DefaultColor& old : DEFAULT_COLORS) {
            if (QLatin1String(old.Token) == QLatin1String(token)) {
                return TestColor(index);
            }
            ++index;
        }
        return {};
    }

    // The items below `item` with the object name, by their visual parents:
    // what a Repeater makes is not a QObject child of anything findChildren
    // reaches.
    static void Collect(QQuickItem* item, const QString& name,
                        QList<QQuickItem*>& found) {
        for (QQuickItem* child : item->childItems()) {
            if (child->objectName() == name) {
                found << child;
            }
            Collect(child, name, found);
        }
    }

    QQuickItem* InRow(int viewRow, const QString& name) {
        return ViewItem(viewRow)->findChild<QQuickItem*>(name);
    }

private slots:
    void init() { Prepare(); }

    void cleanup() { Teardown(); }

    void DefaultTokensAreTheLightStyle() {
        FailOnWarnings();
        const Theme theme;
        for (const DefaultColor& old : DEFAULT_COLORS) {
            const QVariant value = theme.property(old.Token);
            QVERIFY2(value.isValid(), old.Token);
            QCOMPARE(value.value<QColor>(), QColor(old.Value));
        }
        const QVariantList categories = theme.categories();
        QCOMPARE(categories.size(), std::ssize(DEFAULT_CATEGORIES));
        for (int i = 0; i < categories.size(); ++i) {
            QCOMPARE(categories.at(i).value<QColor>(),
                     QColor(DEFAULT_CATEGORIES[i]));
        }
    }

    void EveryTokenIsInTheTable() {
        QStringList known{"categories"};
        for (const DefaultColor& old : DEFAULT_COLORS) {
            known << old.Token;
        }
        known.sort();
        QStringList names = TokenNames();
        names.sort();
        QCOMPARE(names, known);
    }

    // The sizes of the style are constants of the style, the same for every
    // theme.
    void TheSizesAreTheSizesOfTheStyle() {
        FailOnWarnings();
        const Theme theme;
        const QHash<QByteArray, int> sizes{
            {"radiusCard", 12},    {"cornerInset", 5},     {"radiusControl", 6},
            {"radiusSmall", 5},    {"windowMargin", 16},   {"panelGap", 12},
            {"panelPadding", 16},  {"rowHeight", 56},      {"rowGap", 3},
            {"focusRingWidth", 3}, {"cursorRingWidth", 2},
        };
        int found = 0;
        const QMetaObject& meta = Theme::staticMetaObject;
        for (int i = meta.propertyOffset(); i < meta.propertyCount(); ++i) {
            const QMetaProperty property = meta.property(i);
            if (property.metaType() != QMetaType::fromType<int>()) {
                continue;
            }
            ++found;
            QVERIFY2(sizes.contains(property.name()), property.name());
            QVERIFY(property.isConstant());
            QCOMPARE(property.read(&theme).toInt(),
                     sizes.value(property.name()));
        }
        QCOMPARE(found, sizes.size());
    }

    // The type scale is uiFont at a ratio of its size, and it follows
    // settings.uiFontSize.
    void TheTypeScaleIsTheUiFontAtARatio() {
        FailOnWarnings();
        Theme theme;
        QSignalSpy changed(&theme, &Theme::changed);
        theme.followFonts({{"uiFontSize", 13}});
        QCOMPARE(theme.uiFont().pointSizeF(), 13.0);
        QCOMPARE(theme.titleFont().pointSizeF(), 20.0);
        QCOMPARE(theme.titleFont().weight(), QFont::DemiBold);
        QCOMPARE(theme.headingFont().pointSizeF(), 16.0);
        QCOMPARE(theme.headingFont().weight(), QFont::DemiBold);
        QCOMPARE(theme.rowFont().pointSizeF(), 12.0);
        QCOMPARE(theme.rowFont().weight(), QFont::Medium);
        QCOMPARE(theme.metaFont().pointSizeF(), 11.0);
        QCOMPARE(theme.smallFont().pointSizeF(), 10.5);
        QCOMPARE(theme.metaFont().family(), theme.uiFont().family());
        QCOMPARE(theme.metaFont().weight(), theme.uiFont().weight());

        changed.clear();
        theme.followFonts({{"uiFontSize", 26}});
        QCOMPARE(changed.size(), 1);
        QCOMPARE(theme.titleFont().pointSizeF(), 40.0);
        QCOMPARE(theme.rowFont().pointSizeF(), 24.0);
        QCOMPARE(theme.smallFont().pointSizeF(), 21.0);
    }

    // `withWeight` changes the weight and nothing else: the size stays in
    // pixels or in points, fractions too, and the other attributes stay.
    void WithWeightKeepsTheRestOfTheFont() {
        FailOnWarnings();
        QFont points;
        points.setFamily(QStringLiteral("Helvetica"));
        points.setPointSizeF(10.5);
        points.setItalic(true);
        points.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
        const QFont bold = Theme::withWeight(points, QFont::DemiBold);
        QCOMPARE(bold.weight(), QFont::DemiBold);
        QCOMPARE(bold.pointSizeF(), 10.5);
        QCOMPARE(bold.pixelSize(), -1);
        QCOMPARE(bold.family(), points.family());
        QVERIFY(bold.italic());
        QCOMPARE(bold.letterSpacing(), 0.5);

        QFont pixels;
        pixels.setPixelSize(14);
        const QFont heavy = Theme::withWeight(pixels, QFont::Bold);
        QCOMPARE(heavy.pixelSize(), 14);
        QCOMPARE(heavy.pointSizeF(), -1.0);
        QCOMPARE(heavy.weight(), QFont::Bold);

        // Digits of one width, on request only.
        QVERIFY(!Theme::withWeight(pixels, QFont::Normal)
                     .featureTags()
                     .contains(QFont::Tag("tnum")));
        const QFont tabular = Theme::withWeight(pixels, QFont::Normal, true);
        QCOMPARE(tabular.featureValue(QFont::Tag("tnum")), 1U);
        QCOMPARE(tabular.pixelSize(), 14);

        // A weight out of range is the nearest one that exists.
        QCOMPARE(Theme::withWeight(pixels, 5000).weight(), 1000);
        QCOMPARE(Theme::withWeight(pixels, -3).weight(), 1);
    }

    // Text is 4.5:1 against what it lies on, a ring is 3:1 against the surface
    // and the canvas (18-style.md).
    void TheDefaultThemeHasTheContrastOfTheStyleGuide() {
        FailOnWarnings();
        const Theme theme;
        const auto color = [&theme](const char* token) {
            return theme.property(token).value<QColor>();
        };
        struct Pair {
            const char* Front;
            const char* Back;
            double Least;
        };
        const Pair pairs[] = {
            {"text", "surface", 4.5},
            {"text", "canvas", 4.5},
            {"text", "cursorFocused", 4.5},
            {"text", "cursorUnfocused", 4.5},
            {"text", "selectedBackground", 4.5},
            {"text", "hoverBackground", 4.5},
            {"textStrong", "surface", 4.5},
            {"textSecondary", "surface", 4.5},
            {"textSecondary", "canvas", 4.5},
            {"textSecondary", "cursorFocused", 4.5},
            {"textSecondary", "selectedBackground", 4.5},
            {"textSecondary", "hoverBackground", 4.5},
            {"textMuted", "surface", 4.5},
            {"textMuted", "cursorFocused", 4.5},
            {"textMuted", "cursorUnfocused", 4.5},
            {"textMuted", "canvas", 4.5},
            {"textMuted", "selectedBackground", 4.5},
            {"textMuted", "hoverBackground", 4.5},
            {"textMuted", "controlBackground", 4.5},
            {"textSecondary", "cursorUnfocused", 4.5},
            {"stateWorking", "surface", 4.5},
            {"stateWorking", "cursorFocused", 4.5},
            {"stateWorking", "selectedBackground", 4.5},
            {"stateWorking", "canvas", 4.5},
            {"stateBlocked", "surface", 4.5},
            {"stateBlocked", "selectedBackground", 4.5},
            {"stateFailed", "surface", 4.5},
            {"stateFailed", "selectedBackground", 4.5},
            {"accent", "surface", 4.5},
            {"accent", "selectedBackground", 4.5},
            {"attention", "surface", 4.5},
            {"attention", "selectedBackground", 4.5},
            {"attention", "canvas", 4.5},
            {"problemsText", "problemsBackground", 4.5},
            // The text of the strips of the window (derived, no mockup): the
            // banner (`bannerText`, a refusal and a failed start in `error`),
            // the update strip, the toolbar.
            {"bannerText", "bannerBackground", 4.5},
            {"error", "bannerBackground", 4.5},
            {"error", "bannerRefusedBackground", 4.5},
            {"attention", "controlBackground", 4.5},
            {"error", "controlBackground", 4.5},
            {"warning", "surface", 4.5},
            {"error", "surface", 4.5},
            {"windowError", "canvas", 4.5},
            {"primaryText", "primary", 4.5},
            {"primaryText", "primaryHover", 4.5},
            // The palette's hint is text in `focus` (18-style.md).
            {"focus", "surface", 4.5},
            {"focus", "canvas", 3.0},
            {"focus", "cursorFocused", 3.0},
            // Text too: the label of the current filter chip.
            {"focus", "selectedBackground", 4.5},
            // The knob of the switch on its track: not text.
            {"surface", "primary", 3.0},
            {"surface", "textMuted", 3.0},
        };
        for (const Pair& pair : pairs) {
            const double ratio = Contrast(color(pair.Front), color(pair.Back));
            QVERIFY2(ratio >= pair.Least,
                     qPrintable(QStringLiteral("%1 on %2: %3")
                                    .arg(QLatin1String(pair.Front),
                                         QLatin1String(pair.Back))
                                    .arg(ratio)));
        }
    }

    void LoadPutsATokenOverTheCurrentOne() {
        QTest::failOnWarning();
        Theme theme;
        QSignalSpy changed(&theme, &Theme::changed);

        QVERIFY(theme.load({{"accent", "#112233"}}));
        QCOMPARE(theme.accent(), QColor("#112233"));
        QCOMPARE(theme.text(), QColor("#18232e"));
        QCOMPARE(changed.size(), 1);

        // The same value again: nothing changed.
        QVERIFY(theme.load({{"accent", "#112233"}}));
        QCOMPARE(changed.size(), 1);

        QVERIFY(theme.load(TestTheme()));
        QCOMPARE(theme.windowBackground(), Given("windowBackground"));
        QCOMPARE(theme.categories().at(3).value<QColor>(), TestColor(103));
        QCOMPARE(changed.size(), 2);
    }

    void LoadIgnoresWhatItDoesNotUnderstand() {
        Theme theme;
        QSignalSpy changed(&theme, &Theme::changed);

        QTest::ignoreMessage(QtWarningMsg,
                             "roost: theme: unknown token nonsense; ignored");
        QTest::ignoreMessage(QtWarningMsg,
                             "roost: theme: text is not a colour; ignored");
        QTest::ignoreMessage(
            QtWarningMsg, "roost: theme: categories need 8 colours; ignored");
        QVERIFY(!theme.load({{"nonsense", "#112233"},
                             {"text", "not a colour"},
                             {"categories", QJsonArray{"#112233"}},
                             {"accent", "#445566"}}));
        QCOMPARE(theme.text(), QColor("#18232e"));
        QCOMPARE(theme.categories().size(), 8);
        QCOMPARE(theme.categories().at(0).value<QColor>(), QColor("#0087d7"));
        // What was good was taken.
        QCOMPARE(theme.accent(), QColor("#445566"));
        QCOMPARE(changed.size(), 1);
    }

    void TheWindowTakesTheColoursOfTheTokens() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Theme* const theme = Singleton_->theme();
        // Before: the colours of the default theme.
        QCOMPARE(Root()->property("color").value<QColor>(), QColor("#f3f6f8"));
        auto* banner = Root()->findChild<QQuickItem*>("connectionBanner");
        auto* bannerText = Root()->findChild<QQuickItem*>("connectionText");
        auto* strip = Root()->findChild<QQuickItem*>("problemsStrip");
        QVERIFY(banner != nullptr && bannerText != nullptr && strip != nullptr);
        QCOMPARE(ColorOf(banner), QColor("#fff4e0"));
        QCOMPARE(ColorOf(bannerText), QColor("#8b500b"));
        QCOMPARE(ColorOf(strip), QColor("#fff4e0"));
        QCOMPARE(ColorOf(Slot(LIST)->findChild<QQuickItem*>("placeholderText")),
                 QColor("#5f6e80"));

        QVERIFY(theme->load(TestTheme()));

        // The window is the canvas the cards lie on.
        QCOMPARE(Root()->property("color").value<QColor>(), Given("canvas"));
        QCOMPARE(ColorOf(banner), Given("bannerBackground"));
        QCOMPARE(ColorOf(bannerText), Given("bannerText"));
        QCOMPARE(ColorOf(strip), Given("problemsBackground"));
        QCOMPARE(ColorOf(Slot(LIST)->findChild<QQuickItem*>("placeholderText")),
                 Given("textMuted"));
        // The banner of a refusal has a colour of its own.
        Root()->setProperty("connection", "refused");
        QCOMPARE(ColorOf(banner), Given("bannerRefusedBackground"));
    }

    // The strips of the window, the toolbar and the placeholder are the tokens
    // of the style (18-style.md): a failure of the connection is in the colour
    // of the error, the rest of the banner waits for the user (`attention`).
    // The toolbar is built on macOS too, though it is not shown there.
    void TheStripsTakeTheTextColoursOfTheStyle() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        auto* bannerText = Root()->findChild<QQuickItem*>("connectionText");
        QVERIFY(bannerText != nullptr);
        const auto textIs = [&](const char* connection, const char* token) {
            Root()->setProperty("connection", connection);
            QCOMPARE(ColorOf(bannerText), Given(token));
        };
        QVERIFY(Singleton_->theme()->load(TestTheme()));
        // `bannerText` is the contract with the plugins (05-plugins.md).
        textIs("connecting", "bannerText");
        textIs("starting", "bannerText");
        textIs("restarting", "bannerText");
        textIs("disconnected", "bannerText");
        textIs("startFailed", "error");
        textIs("refused", "error");
    }

    void TheToolbarAndThePlaceholderTakeTheTokensOfTheStyle() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Singleton_->theme()->load(TestTheme()));
        auto* bar = Root()->findChild<QQuickItem*>("toolBar");
        QVERIFY(bar != nullptr);
        // A `surface` bar with a hairline of `line` under it.
        const auto* barBackground =
            bar->property("background").value<QQuickItem*>();
        QVERIFY(barBackground != nullptr);
        const QList<QQuickItem*> layers = barBackground->childItems();
        QCOMPARE(layers.size(), 2);
        QCOMPARE(ColorOf(layers.at(0)), Given("surface"));
        QCOMPARE(ColorOf(layers.at(1)), Given("line"));
        QCOMPARE(layers.at(1)->height(), 1.0);
        for (const char* name :
             {"paletteButton", "settingsButton", "reportBugButton"}) {
            auto* button = Root()->findChild<QQuickItem*>(name);
            QVERIFY2(button != nullptr, name);
            QCOMPARE(
                ColorOf(button->property("contentItem").value<QQuickItem*>()),
                Given("textSecondary"));
            const auto* plate =
                button->property("background").value<QQuickItem*>();
            QVERIFY2(plate != nullptr, name);
            QCOMPARE(plate->property("radius").toInt(),
                     Singleton_->theme()->radiusControl());
        }
        // The text of a panel without a panel: `textMuted`, `metaFont`.
        auto* placeholder =
            Slot(LIST)->findChild<QQuickItem*>("placeholderText");
        QVERIFY(placeholder != nullptr);
        QCOMPARE(ColorOf(placeholder), Given("textMuted"));
        QCOMPARE(placeholder->property("font").value<QFont>(),
                 Singleton_->theme()->metaFont());
    }

    // The ring of a button focused by the keyboard lies outside it with a gap:
    // the toolbar has room for it, the window edge and the next item of the
    // column do not cut it.
    void TheFocusRingOfAToolbarButtonIsInsideTheToolbar() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        auto* bar = Root()->findChild<QQuickItem*>("toolBar");
        QVERIFY(bar != nullptr);
        // The bar is not shown on macOS (the native menu bar has the actions);
        // the item is the same, so it is shown here to be laid out.
        bar->setVisible(true);
        for (const char* name :
             {"paletteButton", "settingsButton", "reportBugButton"}) {
            auto* button = Root()->findChild<QQuickItem*>(name);
            QVERIFY2(button != nullptr, name);
            auto* ring = button->findChild<QQuickItem*>("focusRing");
            QVERIFY2(ring != nullptr, name);
            button->forceActiveFocus(Qt::TabFocusReason);
            QTRY_VERIFY_WITH_TIMEOUT(ring->isVisible() && ring->width() > 0,
                                     TIMEOUT_MS);
            QTRY_VERIFY_WITH_TIMEOUT(bar->height() > 0, TIMEOUT_MS);
            const QRectF barRect =
                bar->mapRectToScene(QRectF(0, 0, bar->width(), bar->height()));
            const QRectF ringRect = ring->mapRectToScene(
                QRectF(0, 0, ring->width(), ring->height()));
            QVERIFY2(barRect.contains(ringRect),
                     qPrintable(QStringLiteral("%1 ring %2 bar %3")
                                    .arg(QLatin1String(name))
                                    .arg(QDebug::toString(ringRect))
                                    .arg(QDebug::toString(barRect))));
            // The ring does not reach the hairline of the bar either.
            QVERIFY2(ringRect.bottom() <= barRect.bottom() - 1, name);
            // Nothing later in the column starts above the bottom of the ring.
            for (QQuickItem* next :
                 {Root()->findChild<QQuickItem*>("connectionBanner"),
                  Root()->findChild<QQuickItem*>("layoutView")}) {
                QVERIFY(next != nullptr);
                if (!next->isVisible()) {
                    continue;
                }
                QVERIFY2(next->mapRectToScene(QRectF(0, 0, 1, 1)).top() >=
                             ringRect.bottom(),
                         name);
            }
        }
        QVERIFY(SaveSnapshot(Window(), "toolbar-focused"));
    }

    void TheStripsOfTheWindowCanBeSaved() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Root()->setProperty("connection", "connecting");
        Root()->setProperty("problems",
                            QStringList{"plugin demo: the manifest has no id",
                                        "panel list: cannot load the page"});
        auto* strip = Root()->findChild<QQuickItem*>("problemsStrip");
        auto* banner = Root()->findChild<QQuickItem*>("connectionBanner");
        auto* bannerText = Root()->findChild<QQuickItem*>("connectionText");
        QVERIFY(strip != nullptr && banner != nullptr && bannerText != nullptr);
        const Theme* theme = Singleton_->theme();
        QTRY_VERIFY_WITH_TIMEOUT(strip->isVisible() && strip->height() > 0 &&
                                     banner->isVisible() &&
                                     banner->height() > 0,
                                 TIMEOUT_MS);
        QCOMPARE(ColorOf(bannerText), theme->bannerText());
        QVERIFY(SaveSnapshot(Window(), "banner-and-problems"));
        Root()->setProperty("connection", "refused");
        Root()->setProperty("connectionMessage",
                            "roostd 0.3 does not speak this protocol.");
        QTRY_COMPARE_WITH_TIMEOUT(ColorOf(bannerText), theme->error(),
                                  TIMEOUT_MS);
        QCOMPARE(ColorOf(banner), theme->bannerRefusedBackground());
        QVERIFY(banner->isVisible() && bannerText->isVisible());
        QVERIFY(SaveSnapshot(Window(), "banner-refused"));
    }

    void TheListTakesTheColoursOfTheTokens() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QQuickItem* const panel = PanelOf(LIST);
        const auto titleColor = [this](const QString& id) {
            return ColorOf(InRow(ViewRow(static_cast<int>(Ids().indexOf(id))),
                                 "sessionTitle"));
        };
        const auto detailColor = [this](const QString& id) {
            return ColorOf(InRow(ViewRow(static_cast<int>(Ids().indexOf(id))),
                                 "sessionDetail"));
        };
        // The header of the sessions with no category.
        const int none = ViewRow(0) - 1;
        // The state does not colour the title; a session that waits (BETA)
        // has its second line in the colour of attention.
        QCOMPARE(titleColor(ALPHA), QColor("#18232e"));
        QCOMPARE(titleColor(BETA), QColor("#18232e"));
        QCOMPARE(titleColor(GAMMA), QColor("#18232e"));
        QCOMPARE(detailColor(BETA), QColor("#8b500b"));
        QCOMPARE(detailColor(GAMMA), QColor("#5a6a7a"));
        QCOMPARE(ColorOf(InRow(none, "headerTitle")), QColor("#5a6a7a"));

        QVERIFY(Singleton_->theme()->load(TestTheme()));

        QCOMPARE(titleColor(ALPHA), Given("text"));
        QCOMPARE(titleColor(BETA), Given("text"));
        QCOMPARE(titleColor(GAMMA), Given("text"));
        QCOMPARE(detailColor(BETA), Given("attention"));
        QCOMPARE(detailColor(GAMMA), Given("textSecondary"));
        QCOMPARE(ColorOf(InRow(none, "headerTitle")), Given("textSecondary"));
        QCOMPARE(ColorOf(panel->findChild<QQuickItem*>("sessionsTitle")),
                 Given("text"));
        const QVariantList categories =
            panel->property("categoryColors").toList();
        QCOMPARE(categories.size(), 8);
        for (int i = 0; i < 8; ++i) {
            QCOMPARE(categories.at(i).value<QColor>(), TestColor(100 + i));
        }
        // The current chip is in the colour of the focus, the others are
        // secondary.
        QList<QQuickItem*> tabs;
        Collect(panel, "sessionsTab", tabs);
        QCOMPARE(tabs.size(), 3);
        for (const QQuickItem* tab : tabs) {
            QCOMPARE(ColorOf(tab), tab->property("current").toBool()
                                       ? Given("focus")
                                       : Given("textSecondary"));
        }
    }
};

QTEST_MAIN(ThemeTest)
#include "theme_test.moc"
