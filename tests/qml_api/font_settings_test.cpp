// The fonts follow the document `settings` (design.md §5): terminalFont
// {family, size} and uiFontSize. The window is the real one (the real roostd,
// the real panels, a fake `claude` whose `attach` runs fake_tui), so that a
// font written to the settings is seen where it matters: the cell and the grid
// of the terminal, the size that the TUI reads from its PTY, the fonts of the
// items of the window, and the strip of problems.

#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonObject>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QSignalSpy>
#include <QTest>

#include <memory>

#include "core/theme_source.hpp"
#include "helpers/sessions_window.hpp"
#include "qml_api/theme.hpp"
#include "terminal/renderer.hpp"

using namespace roost;
using namespace roost::testing;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

// A size that is not the default one and gives a visibly bigger cell.
constexpr double BIG_SIZE = 30;

// The family of a font that is installed and fixed pitch, or empty.
QString MonospaceFamily() {
    for (const QString& family : QFontDatabase::families()) {
        if (IsFixedPitch(QFont(family))) {
            return family;
        }
    }
    return {};
}

// The family of a font that is installed and proportional, or empty.
QString ProportionalFamily() {
    for (const QString& family : QFontDatabase::families()) {
        if (!IsFixedPitch(QFont(family))) {
            return family;
        }
    }
    return {};
}

}  // namespace

class FontSettingsTest : public SessionsWindow {
    Q_OBJECT

private:
    void WriteSettings(const QJsonObject& settings) {
        const QFuture<rpc::Result> writing =
            Api_->Store().Set(QStringLiteral("settings"), settings);
        QTRY_VERIFY_WITH_TIMEOUT(writing.isFinished(), TIMEOUT_MS);
        QVERIFY(writing.result());
    }

    static QJsonObject TerminalFont(const QJsonValue& family,
                                    const QJsonValue& size) {
        QJsonObject font;
        if (!family.isUndefined()) {
            font.insert("family", family);
        }
        if (!size.isUndefined()) {
            font.insert("size", size);
        }
        return {{"terminalFont", font}};
    }

    // What the strip of problems shows.
    QStringList Shown() {
        return Root()->property("shownProblems").toStringList();
    }

    // Whether the strip of problems is on the screen.
    bool StripShown() {
        const auto* const strip =
            Root()->findChild<QQuickItem*>("problemsStrip");
        return strip != nullptr && strip->isVisible();
    }

    // The window with a session open in its terminal.
    void BootOpen() {
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(0);
    }

private slots:
    void init() { Prepare(); }

    void cleanup() { Teardown(); }

    // The reader of the settings, with no window: absent keys are the
    // defaults without a problem, and every bad value is a problem.
    void SettingsAreCheckedByTheReader_data() {
        QTest::addColumn<QJsonObject>("settings");
        QTest::addColumn<QString>("family");
        QTest::addColumn<double>("terminalSize");
        QTest::addColumn<double>("uiSize");
        QTest::addColumn<int>("problems");

        QTest::newRow("none") << QJsonObject{} << "" << 0.0 << 0.0 << 0;
        QTest::newRow("all")
            << QJsonObject{{"terminalFont",
                            QJsonObject{{"family", "Foo"}, {"size", 14.5}}},
                           {"uiFontSize", 11}}
            << "Foo" << 14.5 << 11.0 << 0;
        QTest::newRow("size only") << TerminalFont(QJsonValue::Undefined, 16)
                                   << "" << 16.0 << 0.0 << 0;
        QTest::newRow("lower bound")
            << QJsonObject{{"uiFontSize", MIN_FONT_SIZE}} << "" << 0.0
            << MIN_FONT_SIZE << 0;
        QTest::newRow("upper bound")
            << QJsonObject{{"uiFontSize", MAX_FONT_SIZE}} << "" << 0.0
            << MAX_FONT_SIZE << 0;
        QTest::newRow("zero size")
            << TerminalFont("Foo", 0) << "Foo" << 0.0 << 0.0 << 1;
        QTest::newRow("negative size")
            << TerminalFont("Foo", -3) << "Foo" << 0.0 << 0.0 << 1;
        QTest::newRow("below the lower bound")
            << QJsonObject{{"uiFontSize", MIN_FONT_SIZE - 0.5}} << "" << 0.0
            << 0.0 << 1;
        QTest::newRow("absurd size")
            << TerminalFont("Foo", 100000) << "Foo" << 0.0 << 0.0 << 1;
        QTest::newRow("size is a string")
            << TerminalFont("Foo", "12") << "Foo" << 0.0 << 0.0 << 1;
        QTest::newRow("ui size is a bool")
            << QJsonObject{{"uiFontSize", true}} << "" << 0.0 << 0.0 << 1;
        QTest::newRow("family is a number")
            << TerminalFont(12, 14) << "" << 14.0 << 0.0 << 1;
        QTest::newRow("family is empty")
            << TerminalFont("  ", 14) << "" << 14.0 << 0.0 << 1;
        QTest::newRow("terminalFont is a string")
            << QJsonObject{{"terminalFont", "Menlo"}} << "" << 0.0 << 0.0 << 1;
        QTest::newRow("two problems")
            << QJsonObject{{"terminalFont", QJsonObject{{"size", -1}}},
                           {"uiFontSize", "big"}}
            << "" << 0.0 << 0.0 << 2;
    }

    void SettingsAreCheckedByTheReader() {
        QFETCH(QJsonObject, settings);
        QFETCH(QString, family);
        QFETCH(double, terminalSize);
        QFETCH(double, uiSize);
        QFETCH(int, problems);
        const FontSettings fonts = ResolveFontSettings(settings);
        QCOMPARE(fonts.TerminalFamily, family);
        QCOMPARE(fonts.TerminalSize, terminalSize);
        QCOMPARE(fonts.UiSize, uiSize);
        QCOMPARE(fonts.Problems.size(), problems);
    }

    // Without settings the fonts are what the window had before: the system
    // monospace font for the terminal (also what the item has by itself), the
    // application's default font for the window, and no problem.
    void WithoutSettingsTheFontsAreToday() {
        FailOnWarnings();
        const QFont application = QGuiApplication::font();
        BootOpen();
        if (QTest::currentTestFailed()) {
            return;
        }
        const Theme* const theme = Singleton_->theme();
        QCOMPARE(theme->terminalFont(), DefaultTerminalFont());
        QCOMPARE(theme->uiFont(), application);
        QCOMPARE(QGuiApplication::font(), application);
        QCOMPARE(Terminal()->font(), DefaultTerminalFont());
        QVERIFY(theme->fontProblems().isEmpty());
        QVERIFY(Shown().isEmpty());
        QVERIFY(!StripShown());
    }

    // A size written to the settings changes the cell and the grid of the open
    // terminal, and the new grid is the PTY's size: the TUI, which redraws its
    // size line at SIGWINCH, shows it. Removing the key brings the old font
    // back.
    void TheTerminalSizeChangesTheCellTheGridAndThePty() {
        BootOpen();
        if (QTest::currentTestFailed()) {
            return;
        }
        TerminalItem* const terminal = Terminal();
        const qreal cellWidth = terminal->cellWidth();
        const qreal cellHeight = terminal->cellHeight();
        const int columns = terminal->columns();
        const int rows = terminal->rows();
        const QString before =
            QStringLiteral("size %1x%2").arg(columns).arg(rows);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(before), TIMEOUT_MS);

        WriteSettings(TerminalFont(QJsonValue::Undefined, BIG_SIZE));

        QTRY_VERIFY_WITH_TIMEOUT(terminal->cellWidth() > cellWidth, TIMEOUT_MS);
        QVERIFY(terminal->cellHeight() > cellHeight);
        QVERIFY(terminal->columns() < columns);
        QVERIFY(terminal->rows() < rows);
        QCOMPARE(terminal->font().pointSizeF(), BIG_SIZE);
        const QString after = QStringLiteral("size %1x%2")
                                  .arg(terminal->columns())
                                  .arg(terminal->rows());
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(after), TIMEOUT_MS);
        QVERIFY(Shown().isEmpty());

        // The key is gone: today's font, today's grid.
        WriteSettings({});
        QTRY_COMPARE_WITH_TIMEOUT(terminal->cellWidth(), cellWidth, TIMEOUT_MS);
        QCOMPARE(terminal->columns(), columns);
        QCOMPARE(terminal->rows(), rows);
        QCOMPARE(terminal->font(), DefaultTerminalFont());
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(before), TIMEOUT_MS);
    }

    // A family that is installed and monospace is the font of the terminal,
    // at the size of today when the size is not given.
    void AMonospaceFamilyIsTaken() {
        const QString family = MonospaceFamily();
        if (family.isEmpty()) {
            QSKIP("no fixed pitch font is installed");
        }
        BootOpen();
        if (QTest::currentTestFailed()) {
            return;
        }
        WriteSettings(TerminalFont(family, 15));
        QTRY_COMPARE_WITH_TIMEOUT(Terminal()->font().family(), family,
                                  TIMEOUT_MS);
        QCOMPARE(Terminal()->font().pointSizeF(), 15.0);
        QVERIFY(Shown().isEmpty());
    }

    // A proportional family, and a family that is not installed, are the
    // system monospace font, with one problem in the strip; the problem is
    // gone when the setting is fixed.
    void ABadFamilyIsTheSystemMonospaceWithAProblem_data() {
        QTest::addColumn<QString>("family");
        QTest::newRow("proportional") << ProportionalFamily();
        QTest::newRow("missing") << QStringLiteral("No Such Font Family 4711");
    }

    void ABadFamilyIsTheSystemMonospaceWithAProblem() {
        QFETCH(QString, family);
        if (family.isEmpty()) {
            QSKIP("no proportional font is installed");
        }
        BootOpen();
        if (QTest::currentTestFailed()) {
            return;
        }
        TerminalItem* const terminal = Terminal();
        QVERIFY(!StripShown());

        WriteSettings(TerminalFont(family, BIG_SIZE));

        // The size is taken, the family is not.
        QTRY_COMPARE_WITH_TIMEOUT(Shown().size(), 1, TIMEOUT_MS);
        QVERIFY2(Shown().first().contains(family), qPrintable(Shown().first()));
        QVERIFY(StripShown());
        QVERIFY(IsFixedPitch(terminal->font()));
        QCOMPARE(terminal->font().pointSizeF(), BIG_SIZE);
        QCOMPARE(terminal->font().family(), DefaultTerminalFont().family());
        QCOMPARE(Singleton_->theme()->terminalFont(), terminal->font());

        // Fixed: the problem goes, and so does the strip.
        WriteSettings(TerminalFont(QJsonValue::Undefined, BIG_SIZE));
        QTRY_VERIFY_WITH_TIMEOUT(Shown().isEmpty(), TIMEOUT_MS);
        QVERIFY(!StripShown());
    }

    // A size that is not usable is the default size with one problem, the
    // same rule as for the family; the problem goes with a good size.
    void ABadSizeIsTheDefaultSizeWithAProblem_data() {
        QTest::addColumn<QJsonValue>("size");
        QTest::newRow("zero") << QJsonValue(0);
        QTest::newRow("negative") << QJsonValue(-12);
        QTest::newRow("absurd") << QJsonValue(5000);
        QTest::newRow("string") << QJsonValue("twelve");
    }

    void ABadSizeIsTheDefaultSizeWithAProblem() {
        QFETCH(QJsonValue, size);
        BootOpen();
        if (QTest::currentTestFailed()) {
            return;
        }
        WriteSettings(TerminalFont(QJsonValue::Undefined, BIG_SIZE));
        QTRY_COMPARE_WITH_TIMEOUT(Terminal()->font().pointSizeF(), BIG_SIZE,
                                  TIMEOUT_MS);

        WriteSettings({{"terminalFont", QJsonObject{{"size", size}}},
                       {"uiFontSize", size}});

        QTRY_COMPARE_WITH_TIMEOUT(Shown().size(), 2, TIMEOUT_MS);
        QCOMPARE(Terminal()->font(), DefaultTerminalFont());
        QVERIFY(Shown().at(0).contains("settings.terminalFont.size"));
        QVERIFY(Shown().at(1).contains("settings.uiFontSize"));

        WriteSettings(TerminalFont(QJsonValue::Undefined, 14));
        QTRY_VERIFY_WITH_TIMEOUT(Shown().isEmpty(), TIMEOUT_MS);
        QCOMPARE(Terminal()->font().pointSizeF(), 14.0);
    }

    // The UI font is the font of the window: its Controls, and the Labels of
    // the panels, which inherit it.
    void TheUiFontSizeIsTheSizeOfTheWindow() {
        BootOpen();
        if (QTest::currentTestFailed()) {
            return;
        }
        auto* const button = Root()->findChild<QQuickItem*>("retryButton");
        auto* const text =
            Slot(LIST)->findChild<QQuickItem*>("placeholderText");
        auto* const panelText =
            PanelOf(LIST)->findChild<QQuickItem*>("sessionsTitle");
        QVERIFY(button != nullptr && text != nullptr && panelText != nullptr);
        const auto sizeOf = [](const QQuickItem* item) {
            return item->property("font").value<QFont>().pointSizeF();
        };
        const qreal before = sizeOf(button);
        QCOMPARE(sizeOf(text), before);
        const qreal bigger = before + 9;

        WriteSettings({{"uiFontSize", bigger}});

        QTRY_COMPARE_WITH_TIMEOUT(sizeOf(button), bigger, TIMEOUT_MS);
        QCOMPARE(sizeOf(text), bigger);
        QCOMPARE(sizeOf(panelText), bigger);
        QCOMPARE(Singleton_->theme()->uiFont().pointSizeF(), bigger);
        QCOMPARE(QGuiApplication::font().pointSizeF(), before);
        QVERIFY(Shown().isEmpty());
        // The terminal has a font of its own.
        QCOMPARE(Terminal()->font(), DefaultTerminalFont());

        WriteSettings({});
        QTRY_COMPARE_WITH_TIMEOUT(sizeOf(button), before, TIMEOUT_MS);
        QCOMPARE(sizeOf(text), before);
    }

    // Every window follows the document: a second one, over its own API and
    // engine, has the fonts of the same write.
    void AnotherWindowFollowsToo() {
        BootOpen();
        if (QTest::currentTestFailed()) {
            return;
        }
        api::Api other;
        QQmlEngine engine;
        auto* const singleton =
            engine.singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        QVERIFY(singleton != nullptr);
        singleton->setApi(other);
        other.Connect(SocketPath());
        QTRY_VERIFY_WITH_TIMEOUT(other.IsConnected(), TIMEOUT_MS);

        WriteSettings(TerminalFont(QJsonValue::Undefined, BIG_SIZE));

        QTRY_COMPARE_WITH_TIMEOUT(
            singleton->theme()->terminalFont().pointSizeF(), BIG_SIZE,
            TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Terminal()->font().pointSizeF(), BIG_SIZE,
                                  TIMEOUT_MS);
    }
};

QTEST_MAIN(FontSettingsTest)
#include "font_settings_test.moc"
