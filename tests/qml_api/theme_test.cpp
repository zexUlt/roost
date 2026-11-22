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

#include "helpers/sessions_window.hpp"
#include "qml_api/theme.hpp"

using namespace roost;
using namespace roost::testing;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

struct OldColor {
    const char* Token;
    const char* Literal;
};

// Every token and the literal it replaced (the file that had it in brackets).
// A token that is not here fails DefaultTokensAreTheOldLiterals.
constexpr OldColor OLD_COLORS[] = {
    // Main.qml: Window.color was not set, so it was white.
    {"windowBackground", "#ffffff"},
    // SessionsPanel.qml: the background.
    {"panelBackground", "#1e1e1e"},
    // SessionsPanel.qml: the help over the list.
    {"overlayBackground", "#f01e1e1e"},
    // SessionsPanel.qml: textColor; the template's title.
    {"text", "#e0e0e0"},
    // SessionsPanel.qml: the current tab, the confirmation.
    {"textStrong", "#ffffff"},
    // SessionsPanel.qml: the query, the uncategorised header, the marks.
    {"textSecondary", "#a0a0a0"},
    // PanelSlot.qml, TerminalPanel.qml, SessionsPanel.qml: placeholders, the
    // tabs that are not current, the detail.
    {"textMuted", "#808080"},
    // SessionsPanel.qml: the bar of the open session, the unread dot.
    {"accent", "#5aa0ff"},
    {"accentTint", "#225aa0ff"},
    // SessionsPanel.qml: the selection of the prompt.
    {"selection", "#3d5a80"},
    {"cursorFocused", "#3d5a80"},
    {"cursorUnfocused", "#3a3a3a"},
    // SessionsPanel.qml: stateColor.
    {"stateWorking", "#d78700"},
    {"stateBlocked", "#005fd7"},
    {"stateFailed", "#d70000"},
    // Main.qml: the banner and the strip of problems.
    {"bannerBackground", "#4a4a2a"},
    {"bannerRefusedBackground", "#5c1f1f"},
    {"bannerText", "white"},
    {"problemsBackground", "#3a2a1a"},
    {"problemsText", "#ffd9a0"},
    // SessionsPanel.qml: the unavailable agents, the problem of the list.
    {"warning", "#e0a060"},
    {"error", "#e06060"},
    // BugReportDialog.qml: the error on the window.
    {"windowError", "#b00020"},
    // Main.qml: the palette of the window for Qt Quick Controls. No literal
    // had them; they are what the Fusion style paints by default (black text,
    // grey button, white field).
    {"windowText", "#000000"},
    {"controlBackground", "#efefef"},
    {"inputBackground", "#ffffff"},
};

// SessionsPanel.qml: categoryColors.
constexpr const char* OLD_CATEGORIES[] = {"#0087d7", "#d70000", "#008700",
                                          "#d75f00", "#8700d7", "#008787",
                                          "#d700af", "#875f00"};

// The tokens that are colours, by the Q_PROPERTY of Theme; the fonts (and what
// is wrong with them) are not colours, font_settings_test has them.
QStringList TokenNames() {
    QStringList names;
    const QMetaObject& meta = Theme::staticMetaObject;
    for (int i = meta.propertyOffset(); i < meta.propertyCount(); ++i) {
        const QMetaProperty property = meta.property(i);
        if (property.metaType() == QMetaType::fromType<QFont>() ||
            property.metaType() == QMetaType::fromType<QStringList>()) {
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
    for (const OldColor& old : OLD_COLORS) {
        tokens.insert(old.Token, TestColor(index++).name());
    }
    QJsonArray categories;
    for (int i = 0; i < 8; ++i) {
        categories.append(TestColor(100 + i).name());
    }
    tokens.insert("categories", categories);
    return tokens;
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
        for (const OldColor& old : OLD_COLORS) {
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

    void DefaultTokensAreTheOldLiterals() {
        FailOnWarnings();
        const Theme theme;
        for (const OldColor& old : OLD_COLORS) {
            const QVariant value = theme.property(old.Token);
            QVERIFY2(value.isValid(), old.Token);
            QCOMPARE(value.value<QColor>(), QColor(old.Literal));
        }
        const QVariantList categories = theme.categories();
        QCOMPARE(categories.size(), std::ssize(OLD_CATEGORIES));
        for (int i = 0; i < categories.size(); ++i) {
            QCOMPARE(categories.at(i).value<QColor>(),
                     QColor(OLD_CATEGORIES[i]));
        }
    }

    void EveryTokenIsInTheTable() {
        QStringList known{"categories"};
        for (const OldColor& old : OLD_COLORS) {
            known << old.Token;
        }
        known.sort();
        QStringList names = TokenNames();
        names.sort();
        QCOMPARE(names, known);
    }

    void LoadPutsATokenOverTheCurrentOne() {
        QTest::failOnWarning();
        Theme theme;
        QSignalSpy changed(&theme, &Theme::changed);

        QVERIFY(theme.load({{"accent", "#112233"}}));
        QCOMPARE(theme.accent(), QColor("#112233"));
        QCOMPARE(theme.text(), QColor("#e0e0e0"));
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
        QCOMPARE(theme.text(), QColor("#e0e0e0"));
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
        // Before: the colours the window had.
        QCOMPARE(Root()->property("color").value<QColor>(), QColor("#ffffff"));
        auto* banner = Root()->findChild<QQuickItem*>("connectionBanner");
        auto* bannerText = Root()->findChild<QQuickItem*>("connectionText");
        auto* strip = Root()->findChild<QQuickItem*>("problemsStrip");
        QVERIFY(banner != nullptr && bannerText != nullptr && strip != nullptr);
        QCOMPARE(ColorOf(banner), QColor("#4a4a2a"));
        QCOMPARE(ColorOf(bannerText), QColor("white"));
        QCOMPARE(ColorOf(strip), QColor("#3a2a1a"));
        QCOMPARE(ColorOf(Slot(LIST)->findChild<QQuickItem*>("placeholderText")),
                 QColor("#808080"));

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
        QCOMPARE(titleColor(ALPHA), QColor("#ffffff"));
        QCOMPARE(titleColor(BETA), QColor("#005fd7"));
        QCOMPARE(titleColor(GAMMA), QColor("#e0e0e0"));
        QCOMPARE(stateColor("failed"), QColor("#d70000"));
        QCOMPARE(ColorOf(InRow(none, "headerTitle")), QColor("#a0a0a0"));

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
