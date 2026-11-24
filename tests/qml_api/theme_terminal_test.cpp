// The terminal tokens of Roost.theme: the default theme gives the colours of
// the terminals as they were, follow() takes the theme that settings.theme
// names, a bad terminal token changes nothing, and terminalColorsChanged says
// when the colours are others. ROOST_TEST_THEME_FILE is the theme "test".

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "qml_api/theme.hpp"

using namespace roost;

namespace {

using Rgb = TerminalColors::Rgb;

// A theme file with the given tokens; the environment names it.
bool WriteTheme(const QTemporaryDir& dir, const QJsonObject& tokens) {
    QFile file(dir.filePath("theme.json"));
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(tokens).toJson()) < 0) {
        return false;
    }
    qputenv("ROOST_TEST_THEME_FILE", file.fileName().toLocal8Bit());
    return true;
}

}  // namespace

class ThemeTerminalTest : public QObject {
    Q_OBJECT

private slots:
    void cleanup() { qunsetenv("ROOST_TEST_THEME_FILE"); }

    void DefaultsAreTheLightTomorrow() {
        const Theme theme;
        const TerminalColors& colors = theme.terminalColors();
        QCOMPARE(colors.Foreground, (Rgb{.R = 0x4d, .G = 0x4d, .B = 0x4c}));
        QCOMPARE(colors.Background, (Rgb{.R = 255, .G = 255, .B = 255}));
        QCOMPARE(colors.Cursor, (Rgb{.R = 0x4d, .G = 0x4d, .B = 0x4c}));
        QCOMPARE(colors.Selection, (Rgb{.R = 0xd6, .G = 0xd6, .B = 0xd6}));
        QCOMPARE(colors.Palette[1], (Rgb{.R = 0xc8, .G = 0x28, .B = 0x29}));
        QCOMPARE(colors.Palette[15], (Rgb{.R = 0xef, .G = 0xef, .B = 0xef}));
        QCOMPARE(colors.Palette[16], (Rgb{.R = 0, .G = 0, .B = 0}));
        QCOMPARE(colors.Palette[231], (Rgb{.R = 255, .G = 255, .B = 255}));
    }

    void FollowTakesTheThemeThatSettingsNames() {
        const QTemporaryDir dir;
        QVERIFY(WriteTheme(dir, {{"terminalBackground", "#102030"}}));
        Theme theme;
        const QSignalSpy spy(&theme, &Theme::terminalColorsChanged);
        QCOMPARE(theme.follow({{"theme", "test"}}), QStringLiteral("test"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(theme.terminalColors().Background,
                 (Rgb{.R = 0x10, .G = 0x20, .B = 0x30}));
        // What the theme does not give is the default's.
        QCOMPARE(theme.terminalColors().Foreground,
                 (Rgb{.R = 0x4d, .G = 0x4d, .B = 0x4c}));
    }

    void FollowingTheSameThemeAgainSaysNothing() {
        const QTemporaryDir dir;
        QVERIFY(WriteTheme(dir, {{"terminalBackground", "#102030"}}));
        Theme theme;
        theme.follow({{"theme", "test"}});
        const QSignalSpy spy(&theme, &Theme::terminalColorsChanged);
        theme.follow({{"theme", "test"}});
        QCOMPARE(spy.count(), 0);
    }

    void UnknownThemeIsTheDefaultWithAWarning() {
        const QTemporaryDir dir;
        QVERIFY(WriteTheme(dir, {{"terminalBackground", "#102030"}}));
        Theme theme;
        theme.follow({{"theme", "test"}});
        const QSignalSpy spy(&theme, &Theme::terminalColorsChanged);
        QTest::ignoreMessage(
            QtWarningMsg,
            "roost: unknown theme \"nonesuch\"; the default is used");
        QCOMPARE(theme.follow({{"theme", "nonesuch"}}),
                 QStringLiteral("default"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(theme.terminalColors().Background,
                 (Rgb{.R = 255, .G = 255, .B = 255}));
    }

    void ThemeWithoutAnIdIsTheDefault() {
        Theme theme;
        QCOMPARE(theme.follow({}), QStringLiteral("default"));
    }

    void TestThemeNeedsItsFile() {
        Theme theme;
        QTest::ignoreMessage(
            QtWarningMsg, "roost: unknown theme \"test\"; the default is used");
        QCOMPARE(theme.follow({{"theme", "test"}}), QStringLiteral("default"));
    }

    // One bad terminal token and none of the terminal tokens of the call is
    // taken: half a theme is no theme.
    void BadTerminalTokenChangesNoTerminalColour() {
        Theme theme;
        const QSignalSpy spy(&theme, &Theme::terminalColorsChanged);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("terminal"));
        QVERIFY(!theme.load({{"terminalBackground", "#102030"},
                             {"terminalForeground", "red"}}));
        QCOMPARE(spy.count(), 0);
        QCOMPARE(theme.terminalColors().Background,
                 (Rgb{.R = 255, .G = 255, .B = 255}));
    }

    // The terminal tokens are not properties: QML cannot see them, and the
    // properties are the tokens of the window.
    void TerminalTokensAreNotProperties() {
        const Theme theme;
        QCOMPARE(theme.metaObject()->indexOfProperty("terminalBackground"), -1);
    }
};

QTEST_MAIN(ThemeTerminalTest)
#include "theme_terminal_test.moc"
