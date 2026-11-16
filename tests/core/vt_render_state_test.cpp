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

    void PaletteColorComesResolved() {
        auto terminal = NewTerminal();
        terminal.Write("\x1b[31mR");
        VtRenderState state;
        Update(state, terminal);

        const auto rows = state.Rows();
        const auto& cell = rows[0].Cells[0];
        QVERIFY(cell.Foreground.has_value());
        QVERIFY(!(*cell.Foreground == state.DefaultForeground()));
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
