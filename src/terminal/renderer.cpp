#include "terminal/renderer.hpp"

#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QPointF>
#include <QQuickWindow>
#include <QRectF>
#include <QSGNode>
#include <QSGRectangleNode>
#include <QSGTextNode>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextOption>

#include <cmath>
#include <memory>

namespace roost {

namespace {

using Rgb = VtRenderState::Rgb;

// Half of the foreground's opacity: SGR 2.
constexpr int FAINT_ALPHA = 128;
// A block cursor over the glyph must leave the glyph readable.
constexpr int CURSOR_ALPHA = 160;
// A line wider than any terminal row, so Qt never wraps.
constexpr qreal NO_WRAP_WIDTH = 1e6;

QColor ToColor(Rgb rgb, int alpha = 255) {
    return QColor(rgb.R, rgb.G, rgb.B, alpha);
}

struct Resolved {
    QColor Foreground;
    QColor Background;
};

Resolved Resolve(const VtRenderState::Cell& cell, Rgb defaultForeground,
                 Rgb defaultBackground) {
    auto foreground = cell.Foreground.value_or(defaultForeground);
    auto background = cell.Background.value_or(defaultBackground);
    // A selected cell swaps once more, as Ghostty does when no selection
    // colors are set: a cell that is inverse and selected looks plain.
    if (cell.Inverse != cell.Selected) {
        std::swap(foreground, background);
    }
    return {.Foreground = ToColor(foreground, cell.Faint ? FAINT_ALPHA : 255),
            .Background = ToColor(background)};
}

bool IsShown(const VtRenderState::Cell& cell) {
    return !cell.Text.isEmpty() && !cell.Invisible;
}

Run TextRun(int column, const VtRenderState::Cell& cell,
            const Resolved& colors) {
    return {.Column = column,
            .Width = 1,
            .Text = cell.Text,
            .Foreground = colors.Foreground,
            .Background = colors.Background,
            .Bold = cell.Bold,
            .Italic = cell.Italic,
            .Underline = cell.Underline,
            .Strikethrough = cell.Strikethrough};
}

bool SameStyle(const Run& run, const Run& other) {
    return run.Foreground == other.Foreground &&
           run.Background == other.Background && run.Bold == other.Bold &&
           run.Italic == other.Italic && run.Underline == other.Underline &&
           run.Strikethrough == other.Strikethrough;
}

// Whether the face the font resolves to is fixed pitch. The platform's own
// flag is not enough: offscreen, the fixed font can resolve to a proportional
// face that still claims to be fixed, so the advances are compared as well.
bool IsFixedPitch(const QFont& font) {
    if (!QFontInfo(font).fixedPitch()) {
        return false;
    }
    const QFontMetricsF metrics(font);
    return metrics.horizontalAdvance(u'i') == metrics.horizontalAdvance(u'W');
}

}  // namespace

CellMetrics MeasureCell(const QFont& font) {
    const QFontMetricsF metrics(font);
    return {
        .Width = static_cast<int>(std::ceil(metrics.horizontalAdvance(u'M'))),
        .Height = static_cast<int>(std::ceil(metrics.lineSpacing()))};
}

QFont DefaultTerminalFont() {
    const auto system = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (IsFixedPitch(system)) {
        return system;
    }
    // The system's fixed font is not one (offscreen on macOS resolves it to a
    // proportional face). Ask for any monospace face by style hint, then for
    // well-known families; a missing family resolves to the proportional
    // default, which the check rejects.
    const auto sized = [&](QFont font) {
        font.setPointSizeF(system.pointSizeF());
        return font;
    };
    for (const auto hint : {QFont::Monospace, QFont::TypeWriter}) {
        auto font = sized(QFont());
        font.setStyleHint(hint);
        font.setFixedPitch(true);
        if (IsFixedPitch(font)) {
            return font;
        }
    }
    for (const auto* family : {"Menlo", "SF Mono", "Monaco", "DejaVu Sans Mono",
                               "Liberation Mono", "Noto Sans Mono"}) {
        const auto font = sized(QFont(QString::fromLatin1(family)));
        if (IsFixedPitch(font)) {
            return font;
        }
    }
    // No fixed-pitch face anywhere: the system font it is. The letter spacing
    // in Renderer still keeps runs on the grid.
    return system;
}

QList<Run> SplitRow(const VtRenderState::Row& row, Rgb defaultForeground,
                    Rgb defaultBackground) {
    const auto& cells = row.Cells;
    const auto count = static_cast<int>(cells.size());
    QList<Run> runs;

    const auto colorsOf = [&](int at) {
        return Resolve(cells[at], defaultForeground, defaultBackground);
    };
    int at = 0;
    while (at < count) {
        const auto& cell = cells[at];
        const auto colors = colorsOf(at);
        if (!IsShown(cell)) {
            // Only the background matters here, so cells that differ in
            // anything else still make one run.
            Run run{.Column = at,
                    .Width = 1,
                    .Text = {},
                    .Foreground = ToColor(defaultForeground),
                    .Background = colors.Background};
            ++at;
            while (at < count && !IsShown(cells[at]) &&
                   colorsOf(at).Background == run.Background) {
                ++run.Width;
                ++at;
            }
            runs.append(run);
            continue;
        }

        auto run = TextRun(at, cell, colors);
        if (cell.Width == VtRenderState::EWidth::Wide) {
            // A wide character stands alone: whatever the font does with it,
            // the next run still starts after both halves. Its SpacerTail
            // is covered by this run and never drawn.
            run.Width = 2;
            at += 2;
            runs.append(run);
            continue;
        }
        ++at;
        while (at < count && IsShown(cells[at]) &&
               cells[at].Width != VtRenderState::EWidth::Wide) {
            const auto next = TextRun(at, cells[at], colorsOf(at));
            if (!SameStyle(run, next)) {
                break;
            }
            run.Text += next.Text;
            ++run.Width;
            ++at;
        }
        runs.append(run);
    }
    return runs;
}

Renderer::Renderer(const QFont& font) { SetFont(font); }

void Renderer::SetFont(const QFont& font) {
    Font_ = font;
    Metrics_ = MeasureCell(Font_);
    for (int variant = 0; variant < 4; ++variant) {
        QFont styled = Font_;
        styled.setBold((variant & 1) != 0);
        styled.setItalic((variant & 2) != 0);
        // Whatever the face advances, a glyph takes one cell: the grid is the
        // terminal's, not the font's.
        styled.setLetterSpacing(
            QFont::AbsoluteSpacing,
            Metrics_.Width - QFontMetricsF(styled).horizontalAdvance(u'M'));
        Variants_[static_cast<std::size_t>(variant)] = styled;
    }
    Stale_ = true;
}

QSGNode* Renderer::Update(QQuickWindow& window, TerminalSession& session,
                          QSGNode* oldNode) {
    RowsRebuilt_ = 0;
    {
        const auto lock = session.Lock();
        if (const auto begun = session.BeginRenderUpdate(lock); !begun) {
            qWarning("terminal render update failed: %s",
                     qPrintable(begun.error()));
            return oldNode;
        }
    }
    session.EndRenderUpdate();

    const auto& state = session.RenderState();
    const auto dirty = state.Dirty();
    const auto rowCount = static_cast<int>(state.RowCount());
    if (rowCount == 0) {
        // Null makes the scene graph forget oldNode without removing it from
        // the tree, so it stays on screen: delete it.
        delete oldNode;
        return nullptr;
    }

    // The tree is the rows, then the cursor. The root stays the same node for
    // as long as the scene graph keeps it, so there is never an old root to
    // hand back for deletion.
    QSGNode* root = oldNode != nullptr ? oldNode : new QSGNode;
    const bool sameGrid =
        root->childCount() == rowCount + 1 && state.Columns() == Columns_;
    if (dirty == VtRenderState::EDirty::None && sameGrid && !Stale_) {
        // Nothing to redraw. The cursor is checked anyway: whether a move
        // alone marks any row dirty is the library's business.
        UpdateCursor(*root->lastChild(), state);
        return root;
    }

    if (!sameGrid) {
        // A new size: the old rows cannot be told from the new ones.
        while (auto* child = root->firstChild()) {
            root->removeChildNode(child);
            delete child;
        }
        for (int i = 0; i < rowCount; ++i) {
            root->appendChildNode(new QSGNode);
        }
        root->appendChildNode(window.createRectangleNode());
    }

    const bool rebuildAll =
        !sameGrid || Stale_ || dirty == VtRenderState::EDirty::Full;
    const auto rows = state.Rows();
    QSGNode* row = root->firstChild();
    for (int i = 0; i < rowCount; ++i, row = row->nextSibling()) {
        if (!rebuildAll && !rows[i].Dirty) {
            continue;
        }
        BuildRow(window, *row, i, state, rows[i]);
        ++RowsRebuilt_;
    }
    UpdateCursor(*root->lastChild(), state);

    Columns_ = state.Columns();
    Stale_ = false;
    session.CleanRenderState();
    return root;
}

void Renderer::BuildRow(QQuickWindow& window, QSGNode& row, int index,
                        const VtRenderState& state,
                        const VtRenderState::Row& source) const {
    while (auto* child = row.firstChild()) {
        row.removeChildNode(child);
        delete child;
    }

    const auto runs =
        SplitRow(source, state.DefaultForeground(), state.DefaultBackground());
    const qreal top = static_cast<qreal>(index) * Metrics_.Height;

    // Backgrounds first, so that text lies over them. Neighbours of one
    // colour share a rectangle.
    for (qsizetype i = 0; i < runs.size();) {
        const auto& first = runs[i];
        int width = first.Width;
        qsizetype next = i + 1;
        while (next < runs.size() &&
               runs[next].Background == first.Background) {
            width += runs[next].Width;
            ++next;
        }
        auto* rect = window.createRectangleNode();
        rect->setRect(QRectF(static_cast<qreal>(first.Column) * Metrics_.Width,
                             top, static_cast<qreal>(width) * Metrics_.Width,
                             Metrics_.Height));
        rect->setColor(first.Background);
        row.appendChildNode(rect);
        i = next;
    }

    auto* text = window.createTextNode();
    for (const auto& run : runs) {
        if (run.Text.isEmpty()) {
            continue;
        }
        QFont font = Variants_[static_cast<std::size_t>((run.Bold ? 1 : 0) +
                                                        (run.Italic ? 2 : 0))];
        font.setUnderline(run.Underline);
        font.setStrikeOut(run.Strikethrough);

        QTextLayout layout(run.Text, font);
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        layout.setTextOption(option);
        QTextCharFormat format;
        format.setForeground(run.Foreground);
        layout.setFormats({{.start = 0,
                            .length = static_cast<int>(run.Text.size()),
                            .format = format}});
        layout.beginLayout();
        auto line = layout.createLine();
        line.setLineWidth(NO_WRAP_WIDTH);
        line.setPosition(QPointF(0, 0));
        layout.endLayout();
        text->addTextLayout(
            QPointF(static_cast<qreal>(run.Column) * Metrics_.Width, top),
            &layout);
    }
    row.appendChildNode(text);
}

void Renderer::UpdateCursor(QSGNode& cursor, const VtRenderState& state) const {
    auto& rect = static_cast<QSGRectangleNode&>(cursor);
    const auto position = state.GetCursor();
    if (!position.Visible || !position.InViewport) {
        rect.setRect(QRectF());
        return;
    }
    // One block over both cells of a wide character, whichever half the
    // cursor is on.
    int first = position.X;
    int width = 1;
    if (position.WideTail && first > 0) {
        --first;
        width = 2;
    } else if (const auto cell = state.CellAt(position.X, position.Y);
               cell && cell->Width == VtRenderState::EWidth::Wide) {
        width = 2;
    }
    rect.setRect(QRectF(static_cast<qreal>(first) * Metrics_.Width,
                        static_cast<qreal>(position.Y) * Metrics_.Height,
                        static_cast<qreal>(width) * Metrics_.Width,
                        Metrics_.Height));
    rect.setColor(ToColor(state.DefaultForeground(), CURSOR_ALPHA));
}

}  // namespace roost
