// What a render state shows after a known VT stream: the text and styles of
// the cells, the cursor, and which rows need redrawing.

#include "core/vt_render_state.hpp"
#include "core/vt_terminal.hpp"

#include <QList>
#include <QTest>

#include <expected>
#include <optional>

using roost::VtRenderState;
using roost::VtTerminal;

namespace {

constexpr quint16 COLUMNS = 10;
constexpr quint16 ROWS = 5;
// Palette index 1 (SGR 31) of the default palette: #cc6666.
constexpr quint8 RED_R = 204;
constexpr quint8 RED_G = 102;
constexpr quint8 RED_B = 102;

VtTerminal NewTerminal() {
    auto terminal = VtTerminal::Create(COLUMNS, ROWS);
    if (!terminal) {
        qFatal("%s", qPrintable(terminal.error()));
    }
    return std::move(*terminal);
}

void Update(VtRenderState& state, VtTerminal& terminal) {
    const auto begun = state.BeginUpdate(terminal);
    if (!begun) {
        qFatal("%s", qPrintable(begun.error()));
    }
    state.EndUpdate();
}

QList<int> DirtyRows(const VtRenderState& state) {
    QList<int> dirty;
    const auto rows = state.Rows();
    for (int y = 0; y < rows.size(); ++y) {
        if (rows[y].Dirty) {
            dirty.append(y);
        }
    }
    return dirty;
}

}  // namespace

class VtRenderStateTest : public QObject {
    Q_OBJECT

private slots:
    void EmptyStateHasNothing() {
        const VtRenderState state;
        QCOMPARE(state.Columns(), quint16{0});
        QVERIFY(state.Rows().isEmpty());
        QCOMPARE(state.Dirty(), VtRenderState::EDirty::None);
    }

    void TextWideAndCombining() {
        auto terminal = NewTerminal();
        // "A", a wide character, "e" with a combining acute accent, "z".
        terminal.Write(
            QByteArrayView("A\xe6\xbc\xa2"
                           "e\xcc\x81z"));
        VtRenderState state;
        Update(state, terminal);

        QCOMPARE(state.Columns(), COLUMNS);
        const auto rows = state.Rows();
        QCOMPARE(rows.size(), ROWS);
        const auto& cells = rows[0].Cells;
        QCOMPARE(cells.size(), COLUMNS);
        QCOMPARE(cells[0].Text, QStringLiteral("A"));
        QCOMPARE(cells[1].Text, QStringLiteral("漢"));
        // The second half of a wide character has no text of its own.
        QCOMPARE(cells[2].Text, QString());
        QCOMPARE(cells[3].Text, QStringLiteral("é"));
        QCOMPARE(cells[4].Text, QStringLiteral("z"));
        QCOMPARE(cells[5].Text, QString());
    }

    void CellWidthComesFromTheLibrary() {
        auto terminal = NewTerminal();
        // A wide character, "a", then a wide character in the last column
        // of row 0: it does not fit, so the line soft-wraps and the last
        // cell becomes a spacer head.
        terminal.Write(
            QByteArrayView("\xe6\xbc\xa2"
                           "abcdefg\xe6\xbc\xa2"));
        VtRenderState state;
        Update(state, terminal);

        const auto rows = state.Rows();
        using EWidth = VtRenderState::EWidth;
        const auto& cells = rows[0].Cells;
        QCOMPARE(cells[0].Width, EWidth::Wide);
        QCOMPARE(cells[1].Width, EWidth::SpacerTail);
        QCOMPARE(cells[2].Width, EWidth::Narrow);
        QCOMPARE(cells[COLUMNS - 1].Width, EWidth::SpacerHead);
        QCOMPARE(cells[COLUMNS - 1].Text, QString());
        QCOMPARE(rows[1].Cells[0].Width, EWidth::Wide);
        QCOMPARE(rows[1].Cells[0].Text, QStringLiteral("漢"));
    }

    void StylesAndColors() {
        auto terminal = NewTerminal();
        terminal.Write(
            "\x1b[1;38;2;10;200;30;48;2;1;2;3mB\x1b[0m"
            "\x1b[7mI\x1b[0m"
            "\x1b[3;4mU\x1b[0m"
            "P");
        VtRenderState state;
        Update(state, terminal);

        const auto rows = state.Rows();
        const auto& cells = rows[0].Cells;
        const VtRenderState::Rgb fg{.R = 10, .G = 200, .B = 30};
        const VtRenderState::Rgb bg{.R = 1, .G = 2, .B = 3};

        QCOMPARE(cells[0].Text, QStringLiteral("B"));
        QVERIFY(cells[0].Bold);
        QVERIFY(!cells[0].Italic);
        QVERIFY(!cells[0].Underline);
        QVERIFY(!cells[0].Inverse);
        QVERIFY(cells[0].Foreground == fg);
        QVERIFY(cells[0].Background == bg);

        QVERIFY(cells[1].Inverse);
        QVERIFY(!cells[1].Bold);
        QVERIFY(!cells[1].Foreground.has_value());

        QVERIFY(cells[2].Italic);
        QVERIFY(cells[2].Underline);
        QVERIFY(!cells[2].Inverse);

        QCOMPARE(cells[3].Text, QStringLiteral("P"));
        QVERIFY(!cells[3].Bold);
        QVERIFY(!cells[3].Italic);
        QVERIFY(!cells[3].Underline);
        QVERIFY(!cells[3].Inverse);
        QVERIFY(!cells[3].Foreground.has_value());
        QVERIFY(!cells[3].Background.has_value());
    }

    void FaintStrikethroughAndInvisible() {
        auto terminal = NewTerminal();
        terminal.Write(
            "\x1b[2mF\x1b[0m"
            "\x1b[9mS\x1b[0m"
            "\x1b[8mH\x1b[0m"
            "P");
        VtRenderState state;
        Update(state, terminal);

        const auto rows = state.Rows();
        const auto& cells = rows[0].Cells;
        QVERIFY(cells[0].Faint);
        QVERIFY(!cells[0].Strikethrough);
        QVERIFY(!cells[0].Invisible);

        QVERIFY(cells[1].Strikethrough);
        QVERIFY(!cells[1].Faint);
        QVERIFY(!cells[1].Invisible);

        QVERIFY(cells[2].Invisible);
        QVERIFY(!cells[2].Faint);
        QVERIFY(!cells[2].Strikethrough);

        QVERIFY(!cells[3].Faint);
        QVERIFY(!cells[3].Strikethrough);
        QVERIFY(!cells[3].Invisible);
    }

    void PaletteColorComesResolved() {
        auto terminal = NewTerminal();
        terminal.Write("\x1b[31mR");
        VtRenderState state;
        Update(state, terminal);

        const auto rows = state.Rows();
        const auto& cell = rows[0].Cells[0];
        // What libghostty-vt's default palette holds at index 1 (SGR 31),
        // read once from this very library. A palette change in a library
        // upgrade shows up here.
        const VtRenderState::Rgb red{.R = RED_R, .G = RED_G, .B = RED_B};
        QVERIFY(cell.Foreground.has_value());
        QVERIFY(*cell.Foreground == red);
    }

    void LongGraphemeComesWhole() {
        auto terminal = NewTerminal();
        // "e" and 30 combining acute accents: 61 bytes of UTF-8, more than
        // the 32-byte first buffer, so the read takes the out-of-space path.
        QByteArray bytes = "e";
        QString expected = QStringLiteral("e");
        for (int i = 0; i < 30; ++i) {
            bytes += "\xcc\x81";
            expected += QChar(0x0301);
        }
        terminal.Write(bytes);
        VtRenderState state;
        Update(state, terminal);

        const auto rows = state.Rows();
        QCOMPARE(rows[0].Cells[0].Text.size(), 31);
        QCOMPARE(rows[0].Cells[0].Text, expected);
    }

    void NothingIsReadableBetweenBeginAndEnd() {
        auto terminal = NewTerminal();
        terminal.Write("\x1b[31mtext");
        VtRenderState state;
        Update(state, terminal);
        QVERIFY(!state.Rows().isEmpty());

        // The state of the earlier update must not leak out of the next one.
        QVERIFY(state.BeginUpdate(terminal).has_value());
        QCOMPARE(state.Columns(), quint16{0});
        QVERIFY(state.Rows().isEmpty());
        QCOMPARE(state.Dirty(), VtRenderState::EDirty::None);
        const auto cursor = state.GetCursor();
        QVERIFY(!cursor.Visible);
        QVERIFY(!cursor.InViewport);
        QVERIFY(state.DefaultForeground() == VtRenderState::Rgb{});
        QVERIFY(state.DefaultBackground() == VtRenderState::Rgb{});

        state.EndUpdate();
        QCOMPARE(state.Columns(), COLUMNS);
        QCOMPARE(state.Rows().size(), ROWS);
    }

    void TwoStatesOnOneTerminalSplitTheChanges() {
        // Pins the library's behaviour: BeginUpdate consumes the terminal's
        // dirty state, which is why a TerminalSession owns one state only.
        auto terminal = NewTerminal();
        VtRenderState first;
        VtRenderState second;
        Update(first, terminal);
        first.Clean();
        Update(second, terminal);
        second.Clean();

        terminal.Write("x");
        Update(first, terminal);
        Update(second, terminal);
        QCOMPARE(DirtyRows(first), (QList<int>{0}));
        QCOMPARE(second.Dirty(), VtRenderState::EDirty::None);
        QVERIFY(DirtyRows(second).isEmpty());
    }

    // The library sets every row's own flag here as well, so this pins the
    // result rather than the wrapper's Full branch, which no stream tried so
    // far reaches; the branch follows render.h's documented invariant.
    void FullDirtyMarksEveryRow() {
        auto terminal = NewTerminal();
        VtRenderState state;
        Update(state, terminal);
        state.Clean();
        QCOMPARE(state.Dirty(), VtRenderState::EDirty::None);

        QVERIFY(terminal.Resize(20, 8));
        Update(state, terminal);
        QCOMPARE(state.Dirty(), VtRenderState::EDirty::Full);
        QCOMPARE(DirtyRows(state).size(), 8);
    }

    void CursorFollowsCup() {
        auto terminal = NewTerminal();
        terminal.Write("\x1b[3;7H");
        VtRenderState state;
        Update(state, terminal);

        auto cursor = state.GetCursor();
        QVERIFY(cursor.Visible);
        QVERIFY(cursor.InViewport);
        QCOMPARE(cursor.X, quint16{6});
        QCOMPARE(cursor.Y, quint16{2});

        terminal.Write("\x1b[?25l");
        Update(state, terminal);
        QVERIFY(!state.GetCursor().Visible);
    }

    void OnlyRowsWrittenAfterCleanAreDirty() {
        auto terminal = NewTerminal();
        VtRenderState state;
        // The first update of a state is a full redraw.
        Update(state, terminal);
        QCOMPARE(state.Dirty(), VtRenderState::EDirty::Full);
        QCOMPARE(DirtyRows(state).size(), ROWS);
        state.Clean();

        // The cursor ends on row 4, so that row is dirty too.
        terminal.Write("\x1b[1;1Hone\x1b[3;1Hthree\x1b[5;1H");
        Update(state, terminal);
        QCOMPARE(state.Dirty(), VtRenderState::EDirty::Partial);
        QCOMPARE(DirtyRows(state), (QList<int>{0, 2, 4}));

        state.Clean();
        QCOMPARE(state.Dirty(), VtRenderState::EDirty::None);
        QVERIFY(DirtyRows(state).isEmpty());

        terminal.Write("five");
        Update(state, terminal);
        QCOMPARE(DirtyRows(state), (QList<int>{4}));
        // The rows themselves are all still readable.
        const auto rows = state.Rows();
        QCOMPARE(rows[0].Cells[0].Text, QStringLiteral("o"));
    }

    void DirtyStaysUntilCleaned() {
        auto terminal = NewTerminal();
        VtRenderState state;
        Update(state, terminal);
        state.Clean();
        terminal.Write("\x1b[1;1Hone");
        Update(state, terminal);
        terminal.Write("\x1b[5;1Hfive");
        Update(state, terminal);
        QCOMPARE(DirtyRows(state), (QList<int>{0, 4}));
    }

    void FollowsResize() {
        auto terminal = NewTerminal();
        VtRenderState state;
        Update(state, terminal);
        QVERIFY(terminal.Resize(20, 8));
        Update(state, terminal);
        QCOMPARE(state.Columns(), quint16{20});
        QCOMPARE(state.Rows().size(), 8);
    }
};

QTEST_GUILESS_MAIN(VtRenderStateTest)
#include "vt_render_state_test.moc"
