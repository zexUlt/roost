#pragma once

#include <QList>
#include <QString>
#include <QtTypes>

#include <expected>
#include <memory>
#include <optional>

namespace roost {

class VtTerminal;

// A copy of what one frame of a VtTerminal shows: the rows, the cells, the
// cursor (design.md §3). It is read while the terminal goes on taking bytes.
//
// An update has two phases. BeginUpdate reads the terminal and needs
// whatever serializes access to it; EndUpdate finishes the copy and touches
// only this object, so it may run while the terminal is being written. The
// state is read only between EndUpdate and the next BeginUpdate. One state
// is used from one thread at a time.
//
// Dirty flags are set by updates and stay set until Clean, so a frame that
// was not drawn is not forgotten. The terminal's own dirty state is consumed
// by BeginUpdate: two states fed from one terminal split the changes between
// them, and the second one misses what the first took. So there is one state
// per terminal; TerminalSession owns it.
class VtRenderState {
public:
    struct Rgb {
        quint8 R = 0;
        quint8 G = 0;
        quint8 B = 0;

        friend bool operator==(const Rgb&, const Rgb&) = default;
    };

    // How many columns a cell takes, as the library says. A wide character
    // is a Wide cell followed by a SpacerTail; a SpacerHead is the blank
    // cell at the end of a soft-wrapped line that a wide character did not
    // fit into.
    enum class EWidth { Narrow, Wide, SpacerTail, SpacerHead };

    // One grid cell. The second half of a wide character (SpacerTail) has an
    // empty Text.
    // A color the cell does not choose is empty: use the default one.
    // Palette colors come resolved; inverse and bold are not applied to them.
    struct Cell {
        QString Text;
        EWidth Width = EWidth::Narrow;
        std::optional<Rgb> Foreground;
        std::optional<Rgb> Background;
        bool Bold = false;
        bool Italic = false;
        bool Underline = false;
        bool Inverse = false;
        bool Faint = false;
        bool Strikethrough = false;
        bool Invisible = false;
    };

    struct Row {
        // True for every row while Dirty() is Full.
        bool Dirty = false;
        QList<Cell> Cells;
    };

    struct Cursor {
        bool Visible = false;
        // False when the cursor is out of the viewport; X and Y mean nothing.
        bool InViewport = false;
        quint16 X = 0;
        quint16 Y = 0;
        // The cursor is on the second cell of a wide character.
        bool WideTail = false;
    };

    enum class EDirty { None, Partial, Full };

    // Empty: no columns, no rows. The first BeginUpdate takes the terminal's
    // size, and later ones follow its resizes.
    VtRenderState();
    // A moved-from state may only be destroyed or assigned to.
    VtRenderState(VtRenderState&&) noexcept;
    VtRenderState& operator=(VtRenderState&&) noexcept;
    ~VtRenderState();

    // Reads the terminal, which the caller must keep still until this
    // returns. It consumes the terminal's dirty state, hence not const. The
    // state is unreadable until EndUpdate; after a failure it is unchanged
    // or empty.
    std::expected<void, QString> BeginUpdate(VtTerminal& terminal);
    // Safe without a BeginUpdate before it, and while the terminal changes.
    void EndUpdate();
    // Marks everything drawn. Call it only after a whole frame was drawn.
    void Clean();

    // Valid after EndUpdate.
    quint16 Columns() const;
    quint16 RowCount() const;
    EDirty Dirty() const;
    Cursor GetCursor() const;
    Rgb DefaultForeground() const;
    Rgb DefaultBackground() const;
    // Every row of the viewport, top to bottom, with all of its cells.
    QList<Row> Rows() const;
    // One cell of the viewport, without copying the rest; empty when the
    // position is outside it.
    std::optional<Cell> CellAt(quint16 column, quint16 row) const;

private:
    struct State;

    std::unique_ptr<State> State_;
};

}  // namespace roost
