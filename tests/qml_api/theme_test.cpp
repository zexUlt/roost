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
    {"textMuted", "#647385"},
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
    {"stateWorking", "#a35f00"},
    {"stateBlocked", "#8b500b"},
    {"stateFailed", "#c0262d"},
    // The banner and the strip of problems of the window.
    {"bannerBackground", "#fff4e0"},
    {"bannerRefusedBackground", "#fde8e8"},
    {"bannerText", "#18232e"},
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
            {"radiusCard", 12},     {"radiusControl", 6}, {"radiusSmall", 5},
            {"windowMargin", 16},   {"panelGap", 12},     {"panelPadding", 16},
            {"rowHeight", 56},      {"rowGap", 3},        {"focusRingWidth", 3},
            {"cursorRingWidth", 2},
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
            {"stateWorking", "surface", 4.5},
            {"stateWorking", "cursorFocused", 4.5},
            {"stateBlocked", "surface", 4.5},
            {"stateBlocked", "selectedBackground", 4.5},
            {"stateFailed", "surface", 4.5},
            {"stateFailed", "selectedBackground", 4.5},
            {"accent", "surface", 4.5},
            {"accent", "selectedBackground", 4.5},
            {"attention", "surface", 4.5},
            {"attention", "selectedBackground", 4.5},
            {"problemsText", "problemsBackground", 4.5},
            {"bannerText", "bannerBackground", 4.5},
            {"bannerText", "bannerRefusedBackground", 4.5},
            {"warning", "surface", 4.5},
            {"error", "surface", 4.5},
            {"windowError", "canvas", 4.5},
            {"primaryText", "primary", 4.5},
            {"primaryText", "primaryHover", 4.5},
            {"focus", "surface", 3.0},
            {"focus", "canvas", 3.0},
            {"focus", "cursorFocused", 3.0},
            {"focus", "selectedBackground", 3.0},
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
        QCOMPARE(ColorOf(bannerText), QColor("#18232e"));
        QCOMPARE(ColorOf(strip), QColor("#fff4e0"));
        QCOMPARE(ColorOf(Slot(LIST)->findChild<QQuickItem*>("placeholderText")),
                 QColor("#647385"));

        QVERIFY(theme->load(TestTheme()));

        QCOMPARE(Root()->property("color").value<QColor>(),
                 Given("windowBackground"));
        QCOMPARE(ColorOf(banner), Given("bannerBackground"));
        QCOMPARE(ColorOf(bannerText), Given("bannerText"));
        QCOMPARE(ColorOf(strip), Given("problemsBackground"));
        QCOMPARE(ColorOf(Slot(LIST)->findChild<QQuickItem*>("placeholderText")),
                 Given("textMuted"));
        // The banner of a refusal has a colour of its own.
        Root()->setProperty("connection", "refused");
        QCOMPARE(ColorOf(banner), Given("bannerRefusedBackground"));
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
        const auto stateColor = [panel](const char* state) {
            QVariant color;
            QMetaObject::invokeMethod(panel, "stateColor", qReturnArg(color),
                                      QVariant(QString(state)));
            return color.value<QColor>();
        };
        // The header of the sessions with no category.
        const int none = ViewRow(0) - 1;
        // The row under the cursor (ALPHA) is in the strong text colour.
        QCOMPARE(titleColor(ALPHA), QColor("#18232e"));
        QCOMPARE(titleColor(BETA), QColor("#8b500b"));
        QCOMPARE(titleColor(GAMMA), QColor("#18232e"));
        QCOMPARE(stateColor("failed"), QColor("#c0262d"));
        QCOMPARE(ColorOf(InRow(none, "headerTitle")), QColor("#5a6a7a"));

        QVERIFY(Singleton_->theme()->load(TestTheme()));

        QCOMPARE(titleColor(ALPHA), Given("textStrong"));
        QCOMPARE(titleColor(BETA), Given("stateBlocked"));
        QCOMPARE(titleColor(GAMMA), Given("text"));
        QCOMPARE(stateColor("failed"), Given("stateFailed"));
        QCOMPARE(ColorOf(InRow(none, "headerTitle")), Given("textSecondary"));
        QCOMPARE(ColorOf(panel->findChild<QQuickItem*>("sessionsTitle")),
                 Given("text"));
        const QVariantList categories =
            panel->property("categoryColors").toList();
        QCOMPARE(categories.size(), 8);
        for (int i = 0; i < 8; ++i) {
            QCOMPARE(categories.at(i).value<QColor>(), TestColor(100 + i));
        }
        // The current tab is emphasised, the others are muted.
        QList<QQuickItem*> tabs;
        Collect(panel, "sessionsTab", tabs);
        QCOMPARE(tabs.size(), 3);
        for (const QQuickItem* tab : tabs) {
            QCOMPARE(ColorOf(tab), tab->property("current").toBool()
                                       ? Given("textStrong")
                                       : Given("textMuted"));
        }
    }
};

QTEST_MAIN(ThemeTest)
#include "theme_test.moc"
