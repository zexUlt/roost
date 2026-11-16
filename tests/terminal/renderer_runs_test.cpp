// What a known VT stream becomes as runs: the colours resolved, the styles
// split, the grid kept. No fonts and no GPU: SplitRow is pure.

#include "terminal/renderer.hpp"
#include "terminal/terminal_session.hpp"

#include <QColor>
#include <QList>
#include <QString>
#include <QTest>

#include <memory>

using roost::Run;
using roost::TerminalSession;
using roost::VtRenderState;

namespace {

constexpr quint16 COLUMNS = 12;
constexpr quint16 ROWS = 6;

// The row the stream puts at row index `row`, after a render update done the
// way the render thread does it.
QList<Run> RunsOf(QByteArrayView stream, int row = 0,
                  VtRenderState::Rgb* defaultForeground = nullptr,
                  VtRenderState::Rgb* defaultBackground = nullptr) {
    auto created = TerminalSession::Create(COLUMNS, ROWS);
    if (!created) {
        qFatal("%s", qPrintable(created.error()));
    }
    const auto session = std::move(*created);
    {
        const auto lock = session->Lock();
        session->Write(lock, stream);
        if (const auto begun = session->BeginRenderUpdate(lock); !begun) {
            qFatal("%s", qPrintable(begun.error()));
        }
    }
    session->EndRenderUpdate();
    const auto& state = session->RenderState();
    if (defaultForeground != nullptr) {
        *defaultForeground = state.DefaultForeground();
    }
    if (defaultBackground != nullptr) {
        *defaultBackground = state.DefaultBackground();
    }
    return roost::SplitRow(state.Rows().at(row), state.DefaultForeground(),
                           state.DefaultBackground());
}

QColor Color(VtRenderState::Rgb rgb, int alpha = 255) {
    return QColor(rgb.R, rgb.G, rgb.B, alpha);
}

// A text run in the default colours, plain.
Run Plain(int column, const QString& text, int width, QColor foreground,
          QColor background) {
    return {.Column = column,
            .Width = width,
            .Text = text,
            .Foreground = foreground,
            .Background = background};
}

// What is left of the row after `column`: one blank run to the right edge.
Run Rest(int column, QColor foreground, QColor background) {
    return {.Column = column,
            .Width = COLUMNS - column,
            .Text = {},
            .Foreground = foreground,
            .Background = background};
}

}  // namespace

class RendererRunsTest : public QObject {
    Q_OBJECT

private slots:
    void PlainTextIsOneRun() {
        VtRenderState::Rgb fg;
        VtRenderState::Rgb bg;
        const auto runs = RunsOf("hello", 0, &fg, &bg);
        // A word at the end of the written text is one run of its own
        // length; the rest of the row is blank.
        const QList<Run> expected = {
            Plain(0, QStringLiteral("hello"), 5, Color(fg), Color(bg)),
            Rest(5, Color(fg), Color(bg)),
        };
        QCOMPARE(runs, expected);
    }

    void WordsAroundBlankCellsAreSeparateRuns() {
        VtRenderState::Rgb fg;
        VtRenderState::Rgb bg;
        // Three cells nothing was written to, between two words.
        const auto runs = RunsOf("ab\x1b[3Ccd", 0, &fg, &bg);
        const auto f = Color(fg);
        const auto b = Color(bg);
        const QList<Run> expected = {
            Plain(0, QStringLiteral("ab"), 2, f, b),
            {.Column = 2,
             .Width = 3,
             .Text = {},
             .Foreground = f,
             .Background = b},
            Plain(5, QStringLiteral("cd"), 2, f, b),
            Rest(7, f, b),
        };
        QCOMPARE(runs, expected);
    }

    void StylesBreakRuns() {
        VtRenderState::Rgb fg;
        VtRenderState::Rgb bg;
        const auto runs = RunsOf(
            "a\x1b[1mb\x1b[0m\x1b[3mc\x1b[0m\x1b[4md\x1b[0m\x1b[9me\x1b[0m"
            "fg",
            0, &fg, &bg);
        const auto f = Color(fg);
        const auto b = Color(bg);
        auto bold = Plain(1, QStringLiteral("b"), 1, f, b);
        bold.Bold = true;
        auto italic = Plain(2, QStringLiteral("c"), 1, f, b);
        italic.Italic = true;
        auto underline = Plain(3, QStringLiteral("d"), 1, f, b);
        underline.Underline = true;
        auto strike = Plain(4, QStringLiteral("e"), 1, f, b);
        strike.Strikethrough = true;
        const QList<Run> expected = {
            Plain(0, QStringLiteral("a"), 1, f, b),
            bold,
            italic,
            underline,
            strike,
            Plain(5, QStringLiteral("fg"), 2, f, b),
            Rest(7, f, b),
        };
        QCOMPARE(runs, expected);
    }

    void PaletteAndTrueColorAreResolved() {
        VtRenderState::Rgb fg;
        VtRenderState::Rgb bg;
        const auto runs = RunsOf(
            "\x1b[31mr\x1b[38;2;10;200;30;48;2;1;2;3mt\x1b[0m!", 0, &fg, &bg);
        QVERIFY(runs.size() >= 3);
        // Palette 1 of libghostty-vt's default palette.
        const QColor palette(0xcc, 0x66, 0x66);
        QCOMPARE(runs[0].Text, QStringLiteral("r"));
        QCOMPARE(runs[0].Foreground, palette);
        QCOMPARE(runs[0].Background, Color(bg));
        QCOMPARE(runs[1].Text, QStringLiteral("t"));
        QCOMPARE(runs[1].Foreground, QColor(10, 200, 30));
        QCOMPARE(runs[1].Background, QColor(1, 2, 3));
        QCOMPARE(runs[2].Foreground, Color(fg));
    }

    void InverseSwapsTheDefaultColours() {
        VtRenderState::Rgb fg;
        VtRenderState::Rgb bg;
        const auto runs = RunsOf("a\x1b[7mb\x1b[0mc", 0, &fg, &bg);
        QVERIFY(fg != bg);
        QVERIFY(runs.size() >= 3);
        QCOMPARE(runs[0].Foreground, Color(fg));
        QCOMPARE(runs[1].Text, QStringLiteral("b"));
        QCOMPARE(runs[1].Foreground, Color(bg));
        QCOMPARE(runs[1].Background, Color(fg));
        QCOMPARE(runs[2].Foreground, Color(fg));
        QCOMPARE(runs[2].Background, Color(bg));
    }

    void InverseSwapsChosenColours() {
        const auto runs = RunsOf("\x1b[38;2;1;2;3;48;2;4;5;6;7mx");
        QCOMPARE(runs[0].Foreground, QColor(4, 5, 6));
        QCOMPARE(runs[0].Background, QColor(1, 2, 3));
    }

    void FaintHalvesTheForegroundAlpha() {
        VtRenderState::Rgb fg;
        VtRenderState::Rgb bg;
        const auto runs = RunsOf("a\x1b[2mb\x1b[0m", 0, &fg, &bg);
        QVERIFY(runs.size() >= 2);
        QCOMPARE(runs[0].Foreground.alpha(), 255);
        QCOMPARE(runs[1].Text, QStringLiteral("b"));
        QCOMPARE(runs[1].Foreground, Color(fg, 128));
        QCOMPARE(runs[1].Background, Color(bg));
    }

    void InvisibleTextHasNoText() {
        VtRenderState::Rgb fg;
        VtRenderState::Rgb bg;
        const auto runs = RunsOf("a\x1b[8msecret\x1b[0mz", 0, &fg, &bg);
        // The secret keeps its columns and its background, and has no text.
        QCOMPARE(runs[0].Text, QStringLiteral("a"));
        QCOMPARE(runs[1].Text, QString());
        QCOMPARE(runs[1].Column, 1);
        QCOMPARE(runs[1].Width, 6);
        QCOMPARE(runs[2].Text, QStringLiteral("z"));
        QCOMPARE(runs[2].Column, 7);
        for (const auto& run : runs) {
            QVERIFY(!run.Text.contains(u's'));
        }
    }

    void WideCharacterKeepsTheGrid() {
        VtRenderState::Rgb fg;
        VtRenderState::Rgb bg;
        // "a", a wide character at column 1, "b" at column 3.
        const auto runs = RunsOf(
            "a\xe6\xbc\xa2"
            "b",
            0, &fg, &bg);
        const auto f = Color(fg);
        const auto b = Color(bg);
        const QList<Run> expected = {
            Plain(0, QStringLiteral("a"), 1, f, b),
            Plain(1, QStringLiteral("\u6f22"), 2, f, b),
            Plain(3, QStringLiteral("b"), 1, f, b),
            Rest(4, f, b),
        };
        QCOMPARE(runs, expected);
    }

    void CombiningMarkStaysInItsCell() {
        VtRenderState::Rgb fg;
        VtRenderState::Rgb bg;
        const auto runs = RunsOf(
            "e\xcc\x81"
            "xy",
            0, &fg, &bg);
        const auto f = Color(fg);
        const auto b = Color(bg);
        const QList<Run> expected = {
            Plain(0, QStringLiteral("e\u0301xy"), 3, f, b),
            Rest(3, f, b),
        };
        QCOMPARE(runs, expected);
    }

    void BlankCellsWithABackgroundAreRunsToo() {
        VtRenderState::Rgb fg;
        VtRenderState::Rgb bg;
        const auto runs = RunsOf("\x1b[41m  \x1b[0m", 0, &fg, &bg);
        QVERIFY(runs.size() >= 2);
        QCOMPARE(runs[0].Column, 0);
        QCOMPARE(runs[0].Width, 2);
        QVERIFY(runs[0].Background != Color(bg));
        QCOMPARE(runs[1].Column, 2);
        QCOMPARE(runs[1].Background, Color(bg));
    }

    void RunsCoverTheRowOnce() {
        const auto runs = RunsOf("ab \xe6\xbc\xa2 \x1b[1mc\x1b[41m d\x1b[0m");
        int next = 0;
        for (const auto& run : runs) {
            QCOMPARE(run.Column, next);
            QVERIFY(run.Width > 0);
            next += run.Width;
        }
        QCOMPARE(next, int{COLUMNS});
    }
};

QTEST_APPLESS_MAIN(RendererRunsTest)
#include "renderer_runs_test.moc"
