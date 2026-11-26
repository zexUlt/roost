#pragma once

#include "core/vt_render_state.hpp"
#include "terminal/terminal_session.hpp"

#include <QColor>
#include <QFont>
#include <QList>
#include <QString>
#include <QStringList>

#include <array>
#include <atomic>

class QQuickWindow;
class QSGNode;

namespace roost {

// One cell is a whole number of pixels, so that backgrounds, which are
// rectangles, and text, which is laid out by columns, never drift apart.
struct CellMetrics {
    int Width = 0;
    int Height = 0;

    friend bool operator==(const CellMetrics&, const CellMetrics&) = default;
};

// Width of "M" and the line spacing of the font, rounded up.
CellMetrics MeasureCell(const QFont& font);

// Whether the face the font resolves to is fixed pitch.
bool IsFixedPitch(const QFont& font);

// The default font of a terminal: the system's fixed font when it really is
// fixed pitch, else the first monospace face found. Cheap, but not free: call
// it once, not per frame.
QFont DefaultTerminalFont();

// Whether `family` is installed and fixed pitch: a family a terminal can be
// set to. The generic name "monospace" is not a family of the database and is
// not one here.
bool IsInstalledMonospaceFamily(const QString& family);

// The installed families that are fixed pitch, sorted, for the list of the
// settings. A hidden family (a name with a leading dot, such as macOS's
// ".SF NS Mono") is left out. Slow with many fonts: call it when the list is
// opened, not at startup.
QStringList InstalledMonospaceFamilies();

// A stretch of one row drawn in one style. It starts at its exact column and
// spans Width cells; the renderer places it at Column * cell width, so a
// proportional or fallback font cannot shift what follows it.
//
// Text is empty for a stretch that only has a background: blank cells,
// invisible text, and the cells nothing was written to. A wide character is
// a run of its own, Width 2, that spans both of its cells.
struct Run {
    int Column = 0;
    int Width = 1;
    QString Text;
    // Inverse and faint are already applied. Blank runs carry the default
    // foreground and no decoration.
    QColor Foreground;
    QColor Background;
    bool Bold = false;
    bool Italic = false;
    bool Underline = false;
    bool Strikethrough = false;

    friend bool operator==(const Run&, const Run&) = default;
};

// Splits a row into runs that cover every cell of it exactly once, left to
// right. Pure: no Qt Quick, no fonts. A selected cell is drawn on `selection`
// (the theme's terminalSelection), in whichever default colour reads better
// on it, and ignores inverse; any other cell takes its own colours, swapped
// when inverse.
QList<Run> SplitRow(const VtRenderState::Row& row,
                    VtRenderState::Rgb defaultForeground,
                    VtRenderState::Rgb defaultBackground,
                    VtRenderState::Rgb selection);

// Within one run a glyph the primary face lacks, drawn by a fallback face of
// another advance, can shift the rest of that run. What keeps the grid is the
// split between runs (each starts at its exact column, a wide character is a
// run of its own) and, for a monospace primary face, the letter spacing that
// makes a glyph advance exactly one cell. A proportional primary face drifts
// within a run.
//
// Draws one TerminalSession into scene graph nodes. Only the Qt Quick render
// thread uses it, from QQuickItem::updatePaintNode.
//
// It keeps no node of its own: the scene graph owns the tree, and the
// renderer finds its parts again by their place in it (the rows, then the
// cursor). A node it is handed has to be one an earlier Update returned.
class Renderer {
public:
    explicit Renderer(const QFont& font = DefaultTerminalFont());

    // The next frame rebuilds every row. Not safe during Update: the GUI
    // thread calls it while the render thread is not in Update (the scene
    // graph's sync blocks the GUI thread for the duration of one).
    void SetFont(const QFont& font);
    // The next frame rebuilds every row, even when the terminal shows
    // nothing new: for a different session on the same grid. Same thread
    // rule as SetFont.
    void Invalidate() { Stale_ = true; }
    const QFont& Font() const { return Font_; }
    CellMetrics Metrics() const { return Metrics_; }

    // One frame: syncs with the terminal under its lock, then rebuilds what
    // changed. Returns the root node; null when there is nothing to show,
    // after deleting oldNode: the scene graph does not remove a node it is no
    // longer given.
    QSGNode* Update(QQuickWindow& window, TerminalSession& session,
                    QSGNode* oldNode);

    // How many rows the last Update rebuilt. Readable from any thread.
    int RowsRebuilt() const { return RowsRebuilt_; }

private:
    void BuildRow(QQuickWindow& window, QSGNode& row, int index,
                  const VtRenderState& state,
                  const VtRenderState::Row& source) const;
    void UpdateCursor(QSGNode& cursor, const VtRenderState& state) const;

    QFont Font_;
    CellMetrics Metrics_;
    // Bold and italic variants, indexed by Bold + 2 * Italic, each with the
    // letter spacing that makes a glyph advance exactly one cell.
    std::array<QFont, 4> Variants_;
    quint16 Columns_ = 0;
    // The selection colour of the last frame; a new one rebuilds every row.
    VtRenderState::Rgb Selection_{.R = 255, .G = 255, .B = 255};
    bool Stale_ = true;
    std::atomic<int> RowsRebuilt_ = 0;
};

}  // namespace roost
