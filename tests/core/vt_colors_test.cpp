// The colours of a terminal (design.md §5): the colours of the default theme
// (the light Tomorrow, 18-style.md) are what the cells are drawn in and what
// the program is told when it asks, a restored terminal takes them too, and the
// terminal part of a theme is read strictly.

#include "core/terminal_theme.hpp"
#include "core/theme_source.hpp"
#include "core/vt_render_state.hpp"
#include "core/vt_terminal.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QTest>

#include <array>

using roost::ParseTerminalColors;
using roost::ResolveTheme;
using roost::TerminalColors;
using roost::VtRenderState;
using roost::VtTerminal;

namespace {

using Rgb = TerminalColors::Rgb;

constexpr quint16 COLUMNS = 10;
constexpr quint16 ROWS = 3;

VtTerminal NewTerminal() {
    auto terminal = VtTerminal::Create(COLUMNS, ROWS);
    if (!terminal) {
        qFatal("%s", qPrintable(terminal.error()));
    }
    return std::move(*terminal);
}

// Everything the terminal says back is collected here.
struct Replies {
    explicit Replies(VtTerminal& terminal) {
        terminal.SetResponseHandler([this](QByteArrayView response) {
            Bytes += response.toByteArray();
        });
    }
    QByteArray Take() { return std::exchange(Bytes, {}); }
    QByteArray Bytes;
};

void Update(VtRenderState& state, VtTerminal& terminal) {
    if (const auto begun = state.BeginUpdate(terminal); !begun) {
        qFatal("%s", qPrintable(begun.error()));
    }
    state.EndUpdate();
}

// "rgb:rrrr/gggg/bbbb", the way the terminal says a colour: each channel
// twice.
QByteArray Spelled(Rgb color) {
    return QByteArray("rgb:") +
           QByteArray::number(color.R, 16).rightJustified(2, '0').repeated(2) +
           "/" +
           QByteArray::number(color.G, 16).rightJustified(2, '0').repeated(2) +
           "/" +
           QByteArray::number(color.B, 16).rightJustified(2, '0').repeated(2);
}

TerminalColors DefaultColors() {
    const auto colors = ParseTerminalColors(ResolveTheme({}).Tokens);
    if (!colors) {
        qFatal("%s", qPrintable(colors.error()));
    }
    return *colors;
}

// Colours that no default has.
TerminalColors OtherColors() {
    TerminalColors colors = DefaultColors();
    colors.Foreground = {.R = 0x11, .G = 0x22, .B = 0x33};
    colors.Background = {.R = 0xf0, .G = 0xe8, .B = 0xd0};
    colors.Cursor = {.R = 0xaa, .G = 0x00, .B = 0x55};
    colors.Selection = {.R = 0x3d, .G = 0x5a, .B = 0x80};
    colors.Palette[1] = {.R = 12, .G = 34, .B = 56};
    colors.Palette[5] = {.R = 0xab, .G = 0xcd, .B = 0xef};
    colors.Palette[200] = {.R = 1, .G = 2, .B = 3};
    return colors;
}

// A theme with every terminal token, over which a test changes one.
QJsonObject TerminalTokens() {
    QJsonArray ansi;
    for (int i = 0; i < 16; ++i) {
        ansi.append(QStringLiteral("#0000%1").arg(i, 2, 16, QLatin1Char('0')));
    }
    return {{"terminalForeground", "#ffffff"},
            {"terminalBackground", "#000000"},
            {"terminalCursor", "#ff0000"},
            {"terminalSelection", "#00ff00"},
            {"terminalAnsi", ansi}};
}

}  // namespace

class VtColorsTest : public QObject {
    Q_OBJECT

private slots:
    // The colours of the default theme are what the terminal draws and what it
    // answers to a query, for the whole palette.
    void DefaultThemeIsWhatTheTerminalShows() {
        auto terminal = NewTerminal();
        Replies replies(terminal);
        QVERIFY(terminal.SetColors(DefaultColors()));
        VtRenderState state;
        Update(state, terminal);
        const TerminalColors theme = DefaultColors();
        QCOMPARE(state.DefaultForeground(), theme.Foreground);
        QCOMPARE(state.DefaultBackground(), theme.Background);

        // All 256 entries of the palette, as the terminal tells them.
        QByteArray query;
        for (int i = 0; i < 256; ++i) {
            query += "\x1b]4;" + QByteArray::number(i) + ";?\x07";
        }
        terminal.Write(query);
        QByteArray expected;
        for (int i = 0; i < 256; ++i) {
            expected += "\x1b]4;" + QByteArray::number(i) + ";" +
                        Spelled(theme.Palette[static_cast<std::size_t>(i)]) +
                        "\x07";
        }
        QCOMPARE(replies.Take(), expected);
    }

    // What the daemon's terminal does today: asked for the foreground, the
    // background, the cursor or the colour scheme, it says nothing. The
    // reason SetColors is wanted.
    void WithoutColorsNoQueryIsAnswered() {
        auto terminal = NewTerminal();
        Replies replies(terminal);
        terminal.Write("\x1b]10;?\x07\x1b]11;?\x07\x1b]12;?\x07\x1b[?996n");
        QCOMPARE(replies.Take(), QByteArray());
    }

    void PaletteColoursTheCells() {
        auto terminal = NewTerminal();
        QVERIFY(terminal.SetColors(OtherColors()));
        terminal.Write("\x1b[31mx\x1b[35my\x1b[38;5;200mz");
        VtRenderState state;
        Update(state, terminal);
        QCOMPARE(state.CellAt(0, 0)->Foreground, OtherColors().Palette[1]);
        QCOMPARE(state.CellAt(1, 0)->Foreground, OtherColors().Palette[5]);
        QCOMPARE(state.CellAt(2, 0)->Foreground, OtherColors().Palette[200]);
    }

    void DefaultsAndCursorFollowTheColours() {
        auto terminal = NewTerminal();
        QVERIFY(terminal.SetColors(OtherColors()));
        VtRenderState state;
        Update(state, terminal);
        QCOMPARE(state.DefaultForeground(), OtherColors().Foreground);
        QCOMPARE(state.DefaultBackground(), OtherColors().Background);
        QCOMPARE(state.CursorColor(), std::optional(OtherColors().Cursor));
    }

    void ForegroundBackgroundAndCursorQueriesAreAnswered() {
        auto terminal = NewTerminal();
        Replies replies(terminal);
        QVERIFY(terminal.SetColors(OtherColors()));
        terminal.Write("\x1b]10;?\x07");
        QCOMPARE(replies.Take(),
                 "\x1b]10;" + Spelled(OtherColors().Foreground) + "\x07");
        terminal.Write("\x1b]11;?\x07");
        QCOMPARE(replies.Take(),
                 "\x1b]11;" + Spelled(OtherColors().Background) + "\x07");
        terminal.Write("\x1b]12;?\x07");
        QCOMPARE(replies.Take(),
                 "\x1b]12;" + Spelled(OtherColors().Cursor) + "\x07");
    }

    void PaletteQueryIsAnswered() {
        auto terminal = NewTerminal();
        Replies replies(terminal);
        QVERIFY(terminal.SetColors(OtherColors()));
        terminal.Write("\x1b]4;5;?\x07");
        QCOMPARE(replies.Take(),
                 "\x1b]4;5;" + Spelled(OtherColors().Palette[5]) + "\x07");
    }

    // The answer ends as the question did.
    void StringTerminatorIsAnsweredWithOne() {
        auto terminal = NewTerminal();
        Replies replies(terminal);
        QVERIFY(terminal.SetColors(OtherColors()));
        terminal.Write("\x1b]11;?\x1b\\");
        QCOMPARE(replies.Take(),
                 "\x1b]11;" + Spelled(OtherColors().Background) + "\x1b\\");
    }

    void ColourSchemeReportFollowsTheBackground() {
        auto terminal = NewTerminal();
        Replies replies(terminal);
        // The default theme has a light background, as OtherColors does.
        QVERIFY(terminal.SetColors(DefaultColors()));
        terminal.Write("\x1b[?996n");
        QCOMPARE(replies.Take(), QByteArray("\x1b[?997;2n"));
        QVERIFY(terminal.SetColors(OtherColors()));
        terminal.Write("\x1b[?996n");
        QCOMPARE(replies.Take(), QByteArray("\x1b[?997;2n"));
        TerminalColors dark = OtherColors();
        dark.Background = {.R = 0x10, .G = 0x20, .B = 0x30};
        QVERIFY(terminal.SetColors(dark));
        terminal.Write("\x1b[?996n");
        QCOMPARE(replies.Take(), QByteArray("\x1b[?997;1n"));
    }

    // The next colours replace the first: no answer is from the old ones.
    void NewColoursReplaceOldOnes() {
        auto terminal = NewTerminal();
        Replies replies(terminal);
        QVERIFY(terminal.SetColors(OtherColors()));
        QVERIFY(terminal.SetColors(DefaultColors()));
        terminal.Write("\x1b]11;?\x07");
        QCOMPARE(replies.Take(),
                 "\x1b]11;" + Spelled(DefaultColors().Background) + "\x07");
    }

    // A frame already drawn is drawn again in the new colours.
    void NewColoursRedrawEveryRow() {
        auto terminal = NewTerminal();
        terminal.Write("abc\r\ndef");
        VtRenderState state;
        Update(state, terminal);
        state.Clean();
        Update(state, terminal);
        QCOMPARE(state.Dirty(), VtRenderState::EDirty::None);

        QVERIFY(terminal.SetColors(OtherColors()));
        Update(state, terminal);
        QCOMPARE(state.Dirty(), VtRenderState::EDirty::Full);
        QCOMPARE(state.DefaultBackground(), OtherColors().Background);
    }

    // The window's terminal is a restored copy of the daemon's, with colours
    // of the daemon's: it takes its own at once.
    void RestoredTerminalTakesTheColours() {
        auto source = NewTerminal();
        QVERIFY(source.SetColors(DefaultColors()));
        source.Write("\x1b[31mx");
        const auto snapshot = source.Snapshot();
        QVERIFY(snapshot.has_value());
        auto restored = VtTerminal::FromSnapshot(*snapshot);
        QVERIFY(restored.has_value());
        QVERIFY(restored->SetColors(OtherColors()));
        restored->Write("\x1b[31my");
        VtRenderState state;
        Update(state, *restored);
        // The cell that came with the snapshot and one written after it.
        QCOMPARE(state.CellAt(0, 0)->Foreground, OtherColors().Palette[1]);
        QCOMPARE(state.CellAt(1, 0)->Foreground, OtherColors().Palette[1]);
        QCOMPARE(state.DefaultBackground(), OtherColors().Background);
    }

    void ThemeWithEveryTokenParses() {
        const auto colors = ParseTerminalColors(TerminalTokens());
        QVERIFY(colors.has_value());
        QCOMPARE(colors->Cursor, (Rgb{.R = 255, .G = 0, .B = 0}));
        QCOMPARE(colors->Selection, (Rgb{.R = 0, .G = 255, .B = 0}));
        QCOMPARE(colors->Palette[15], (Rgb{.R = 0, .G = 0, .B = 15}));
        // Not given: the cube and the greys.
        QCOMPARE(colors->Palette[196], (Rgb{.R = 255, .G = 0, .B = 0}));
        QCOMPARE(colors->Palette[255], (Rgb{.R = 238, .G = 238, .B = 238}));
    }

    void FullPaletteIsTheBaseOfTheAnsiColours() {
        QJsonObject tokens = TerminalTokens();
        QJsonArray palette;
        for (int i = 0; i < 256; ++i) {
            palette.append(
                QStringLiteral("#01%1ff").arg(i, 2, 16, QLatin1Char('0')));
        }
        tokens.insert("terminalPalette", palette);
        const auto colors = ParseTerminalColors(tokens);
        QVERIFY(colors.has_value());
        QCOMPARE(colors->Palette[100], (Rgb{.R = 1, .G = 100, .B = 255}));
        // terminalAnsi goes over the first sixteen.
        QCOMPARE(colors->Palette[3], (Rgb{.R = 0, .G = 0, .B = 3}));
    }

    void BrokenTokensAreRefused_data() {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QJsonValue>("value");
        QTest::addColumn<QString>("named");
        QTest::newRow("missing background")
            << "terminalBackground" << QJsonValue(QJsonValue::Undefined)
            << "terminalBackground";
        QTest::newRow("name, not hex")
            << "terminalForeground" << QJsonValue("red")
            << "terminalForeground";
        QTest::newRow("alpha")
            << "terminalCursor" << QJsonValue("#ffffff80") << "terminalCursor";
        QTest::newRow("plus sign")
            << "terminalForeground" << QJsonValue("#+12345")
            << "terminalForeground";
        QTest::newRow("space") << "terminalForeground" << QJsonValue("# 12345")
                               << "terminalForeground";
        QTest::newRow("not hex")
            << "terminalForeground" << QJsonValue("#12345g")
            << "terminalForeground";
        QTest::newRow("not a string")
            << "terminalSelection" << QJsonValue(7) << "terminalSelection";
        QTest::newRow("short ansi")
            << "terminalAnsi" << QJsonValue(QJsonArray{"#000000"})
            << "terminalAnsi";
        QTest::newRow("short palette")
            << "terminalPalette" << QJsonValue(QJsonArray{"#000000"})
            << "terminalPalette";
    }

    void BrokenTokensAreRefused() {
        QFETCH(QString, key);
        QFETCH(QJsonValue, value);
        QFETCH(QString, named);
        QJsonObject tokens = TerminalTokens();
        if (value.isUndefined()) {
            tokens.remove(key);
        } else {
            tokens.insert(key, value);
        }
        const auto colors = ParseTerminalColors(tokens);
        QVERIFY(!colors.has_value());
        QVERIFY2(colors.error().contains(named), qPrintable(colors.error()));
    }

    void DarkIsWhereWhiteTextReadsBetterThanBlack() {
        QVERIFY(roost::IsDark({.R = 0, .G = 0, .B = 0}));
        QVERIFY(roost::IsDark({.R = 0x1d, .G = 0x1f, .B = 0x21}));
        QVERIFY(!roost::IsDark({.R = 255, .G = 255, .B = 255}));
        QVERIFY(!roost::IsDark({.R = 0xf0, .G = 0xe8, .B = 0xd0}));
    }
};

QTEST_APPLESS_MAIN(VtColorsTest)
#include "vt_colors_test.moc"
