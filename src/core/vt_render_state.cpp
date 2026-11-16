#include "core/vt_render_state.hpp"

#include "core/ghostty_result.hpp"
#include "core/vt_terminal.hpp"

#include <ghostty/vt.h>

#include <array>
#include <cstdint>
#include <utility>

namespace roost {

namespace {

VtRenderState::Rgb FromLibrary(GhosttyColorRgb color) {
    return {.R = color.r, .G = color.g, .B = color.b};
}

}  // namespace

struct VtRenderState::State {
    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State() {
        ghostty_render_state_row_cells_free(Cells);
        ghostty_render_state_row_iterator_free(Iterator);
        ghostty_render_state_free(Render);
    }

    // Created together, so that reading a row cannot fail for memory.
    std::expected<void, QString> Allocate() {
        if (const auto result = ghostty_render_state_new(nullptr, &Render);
            result != GHOSTTY_SUCCESS) {
            return std::unexpected(Failed("cannot create a state", result));
        }
        if (const auto result =
                ghostty_render_state_row_iterator_new(nullptr, &Iterator);
            result != GHOSTTY_SUCCESS) {
            return std::unexpected(Failed("cannot create a state", result));
        }
        if (const auto result =
                ghostty_render_state_row_cells_new(nullptr, &Cells);
            result != GHOSTTY_SUCCESS) {
            return std::unexpected(Failed("cannot create a state", result));
        }
        return {};
    }

    template <typename TValue>
    TValue Get(GhosttyRenderStateData what) const {
        TValue value{};
        ghostty_render_state_get(Render, what, &value);
        return value;
    }

    VtRenderState::Cell ReadCell() const {
        VtRenderState::Cell cell;
        std::array<std::uint8_t, 32> small{};
        GhosttyBuffer text{.ptr = small.data(), .cap = small.size(), .len = 0};
        QByteArray large;
        auto result = ghostty_render_state_row_cells_get(
            Cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &text);
        if (result == GHOSTTY_OUT_OF_SPACE) {
            large.resize(static_cast<qsizetype>(text.len));
            text = {.ptr = reinterpret_cast<std::uint8_t*>(large.data()),
                    .cap = text.len,
                    .len = 0};
            result = ghostty_render_state_row_cells_get(
                Cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8,
                &text);
        }
        if (result == GHOSTTY_SUCCESS) {
            cell.Text =
                QString::fromUtf8(reinterpret_cast<const char*>(text.ptr),
                                  static_cast<qsizetype>(text.len));
        }

        GhosttyColorRgb color{};
        if (ghostty_render_state_row_cells_get(
                Cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &color) ==
            GHOSTTY_SUCCESS) {
            cell.Foreground = FromLibrary(color);
        }
        if (ghostty_render_state_row_cells_get(
                Cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &color) ==
            GHOSTTY_SUCCESS) {
            cell.Background = FromLibrary(color);
        }

        GhosttyStyle style{};
        if (ghostty_render_state_row_cells_get(
                Cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style) ==
            GHOSTTY_SUCCESS) {
            cell.Bold = (style.flags & GHOSTTY_STYLE_FLAG_BOLD) != 0;
            cell.Italic = (style.flags & GHOSTTY_STYLE_FLAG_ITALIC) != 0;
            cell.Faint = (style.flags & GHOSTTY_STYLE_FLAG_FAINT) != 0;
            cell.Inverse = (style.flags & GHOSTTY_STYLE_FLAG_INVERSE) != 0;
            cell.Invisible = (style.flags & GHOSTTY_STYLE_FLAG_INVISIBLE) != 0;
            cell.Strikethrough =
                (style.flags & GHOSTTY_STYLE_FLAG_STRIKETHROUGH) != 0;
            cell.Underline =
                GHOSTTY_STYLE_UNDERLINE(style) != GHOSTTY_SGR_UNDERLINE_NONE;
        }
        return cell;
    }

    GhosttyRenderState Render = nullptr;
    GhosttyRenderStateRowIterator Iterator = nullptr;
    GhosttyRenderStateRowCells Cells = nullptr;
    // Between BeginUpdate and EndUpdate the state is incomplete.
    bool Updating = false;
};

VtRenderState::VtRenderState() = default;
VtRenderState::VtRenderState(VtRenderState&&) noexcept = default;
VtRenderState& VtRenderState::operator=(VtRenderState&&) noexcept = default;
VtRenderState::~VtRenderState() = default;

std::expected<void, QString> VtRenderState::BeginUpdate(VtTerminal& terminal) {
    if (!State_) {
        auto state = std::make_unique<State>();
        if (auto allocated = state->Allocate(); !allocated) {
            return allocated;
        }
        State_ = std::move(state);
    }
    if (const auto result = ghostty_render_state_begin_update(
            State_->Render,
            static_cast<GhosttyTerminal>(terminal.NativeHandle()));
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot begin an update", result));
    }
    State_->Updating = true;
    return {};
}

void VtRenderState::EndUpdate() {
    if (!State_) {
        return;
    }
    ghostty_render_state_end_update(State_->Render);
    State_->Updating = false;
}

void VtRenderState::Clean() {
    if (State_) {
        ghostty_render_state_clean(State_->Render);
    }
}

quint16 VtRenderState::Columns() const {
    if (!State_ || State_->Updating) {
        return 0;
    }
    return State_->Get<std::uint16_t>(GHOSTTY_RENDER_STATE_DATA_COLS);
}

VtRenderState::EDirty VtRenderState::Dirty() const {
    if (!State_ || State_->Updating) {
        return EDirty::None;
    }
    switch (
        State_->Get<GhosttyRenderStateDirty>(GHOSTTY_RENDER_STATE_DATA_DIRTY)) {
        case GHOSTTY_RENDER_STATE_DIRTY_FALSE:
            return EDirty::None;
        case GHOSTTY_RENDER_STATE_DIRTY_PARTIAL:
            return EDirty::Partial;
        default:
            return EDirty::Full;
    }
}

VtRenderState::Cursor VtRenderState::GetCursor() const {
    Cursor cursor;
    if (!State_ || State_->Updating) {
        return cursor;
    }
    auto raw = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    if (ghostty_render_state_get(State_->Render,
                                 GHOSTTY_RENDER_STATE_DATA_CURSOR,
                                 &raw) == GHOSTTY_SUCCESS) {
        cursor.Visible = raw.visible;
        cursor.InViewport = raw.viewport_has_value;
        if (raw.viewport_has_value) {
            cursor.X = raw.viewport_x;
            cursor.Y = raw.viewport_y;
        }
    }
    return cursor;
}

VtRenderState::Rgb VtRenderState::DefaultForeground() const {
    if (!State_ || State_->Updating) {
        return {};
    }
    return FromLibrary(State_->Get<GhosttyColorRgb>(
        GHOSTTY_RENDER_STATE_DATA_COLOR_FOREGROUND));
}

VtRenderState::Rgb VtRenderState::DefaultBackground() const {
    if (!State_ || State_->Updating) {
        return {};
    }
    return FromLibrary(State_->Get<GhosttyColorRgb>(
        GHOSTTY_RENDER_STATE_DATA_COLOR_BACKGROUND));
}

QList<VtRenderState::Row> VtRenderState::Rows() const {
    QList<Row> rows;
    if (!State_ || State_->Updating) {
        return rows;
    }
    if (ghostty_render_state_get(State_->Render,
                                 GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                 &State_->Iterator) != GHOSTTY_SUCCESS) {
        return rows;
    }
    // After a full redraw the per-row flags mean nothing: every row is dirty.
    const bool all = Dirty() == EDirty::Full;
    while (ghostty_render_state_row_iterator_next(State_->Iterator)) {
        Row row;
        bool dirty = false;
        if (ghostty_render_state_row_get(State_->Iterator,
                                         GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY,
                                         &dirty) == GHOSTTY_SUCCESS) {
            row.Dirty = dirty;
        }
        row.Dirty = row.Dirty || all;
        if (ghostty_render_state_row_get(State_->Iterator,
                                         GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                         &State_->Cells) == GHOSTTY_SUCCESS) {
            while (ghostty_render_state_row_cells_next(State_->Cells)) {
                row.Cells.append(State_->ReadCell());
            }
        }
        rows.append(std::move(row));
    }
    return rows;
}

}  // namespace roost
