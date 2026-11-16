#include "core/vt_terminal.hpp"

#include "core/ghostty_result.hpp"

#include <ghostty/vt.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace roost {

namespace {

bool AppendTo(void* userdata, const uint8_t* data, size_t len) {
    static_cast<QByteArray*>(userdata)->append(
        reinterpret_cast<const char*>(data), static_cast<qsizetype>(len));
    return true;
}

const uint8_t* Bytes(QByteArrayView bytes) {
    return reinterpret_cast<const uint8_t*>(bytes.data());
}

std::expected<void, QString> CheckSize(quint16 columns, quint16 rows) {
    if (columns == 0 || rows == 0 || columns > VtTerminal::MAX_SIZE ||
        rows > VtTerminal::MAX_SIZE) {
        return std::unexpected(
            QStringLiteral("the size must be from 1 to %1 columns and rows")
                .arg(VtTerminal::MAX_SIZE));
    }
    return {};
}

using SelectionEvent =
    std::unique_ptr<std::remove_pointer_t<GhosttySelectionGestureEvent>,
                    decltype(&ghostty_selection_gesture_event_free)>;

std::expected<SelectionEvent, QString> NewEvent(
    GhosttySelectionGestureEventType type) {
    GhosttySelectionGestureEvent raw = nullptr;
    if (const auto result =
            ghostty_selection_gesture_event_new(nullptr, &raw, type);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot create a gesture event", result));
    }
    return SelectionEvent(raw, &ghostty_selection_gesture_event_free);
}

std::expected<void, QString> SetOption(
    const SelectionEvent& event, GhosttySelectionGestureEventOption option,
    const void* value) {
    if (const auto result =
            ghostty_selection_gesture_event_set(event.get(), option, value);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot set a gesture option", result));
    }
    return {};
}

}  // namespace

struct VtTerminal::State {
    explicit State(GhosttyTerminal terminal) : Terminal(terminal) {}
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State() {
        // The gesture tracks cells of the terminal and releases them there.
        ghostty_selection_gesture_free(Gesture, Terminal);
        ghostty_terminal_free(Terminal);
    }

    // The gesture is made on first use: a daemon's terminal never has one.
    std::expected<GhosttySelectionGesture, QString> EnsureGesture() {
        if (Gesture == nullptr) {
            if (const auto result =
                    ghostty_selection_gesture_new(nullptr, &Gesture);
                result != GHOSTTY_SUCCESS) {
                Gesture = nullptr;
                return std::unexpected(
                    Failed("cannot create a selection gesture", result));
            }
        }
        return Gesture;
    }

    // The columns and rows the terminal has now.
    std::expected<std::pair<quint16, quint16>, QString> Size() const {
        std::uint16_t columns = 0;
        std::uint16_t rows = 0;
        if (const auto result = ghostty_terminal_get(
                Terminal, GHOSTTY_TERMINAL_DATA_COLS, &columns);
            result != GHOSTTY_SUCCESS) {
            return std::unexpected(Failed("cannot read the size", result));
        }
        if (const auto result = ghostty_terminal_get(
                Terminal, GHOSTTY_TERMINAL_DATA_ROWS, &rows);
            result != GHOSTTY_SUCCESS) {
            return std::unexpected(Failed("cannot read the size", result));
        }
        return std::pair<quint16, quint16>{columns, rows};
    }

    // The viewport cell nearest to the pointer.
    std::expected<GhosttyPointCoordinate, QString> CellUnder(
        const VtTerminal::SelectionPointer& pointer,
        const MouseGeometry& geometry) const {
        const auto size = Size();
        if (!size) {
            return std::unexpected(size.error());
        }
        const auto nearest = [](double position, quint32 cell, quint16 count) {
            const double at = cell > 0 ? std::floor(position / cell) : 0;
            return static_cast<std::uint32_t>(
                std::clamp(at, 0.0, static_cast<double>(count) - 1));
        };
        return GhosttyPointCoordinate{
            .x = static_cast<std::uint16_t>(
                nearest(pointer.X, geometry.CellWidth, size->first)),
            .y = nearest(pointer.Y, geometry.CellHeight, size->second)};
    }

    std::expected<GhosttyGridRef, QString> RefUnder(
        const VtTerminal::SelectionPointer& pointer,
        const MouseGeometry& geometry) const {
        const auto cell = CellUnder(pointer, geometry);
        if (!cell) {
            return std::unexpected(cell.error());
        }
        GhosttyPoint point{};
        point.tag = GHOSTTY_POINT_TAG_VIEWPORT;
        point.value.coordinate = *cell;
        GhosttyGridRef ref{};
        if (const auto result =
                ghostty_terminal_grid_ref(Terminal, point, &ref);
            result != GHOSTTY_SUCCESS) {
            return std::unexpected(Failed("cannot find the cell", result));
        }
        return ref;
    }

    // What the options of a drag and a tick share.
    std::expected<void, QString> SetGeometry(
        const SelectionEvent& event,
        const VtTerminal::SelectionPointer& pointer,
        const MouseGeometry& geometry) const {
        const auto size = Size();
        if (!size) {
            return std::unexpected(size.error());
        }
        const GhosttySelectionGestureGeometry shape{
            .columns = size->first,
            .cell_width = geometry.CellWidth,
            .padding_left = 0,
            .screen_height = geometry.ScreenHeight};
        if (auto set = SetOption(
                event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_GEOMETRY, &shape);
            !set) {
            return set;
        }
        const GhosttySurfacePosition position{.x = pointer.X, .y = pointer.Y};
        return SetOption(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION,
                         &position);
    }

    // Applies the event. A selection the gesture makes becomes the
    // terminal's, and the result says whether there was one.
    std::expected<bool, QString> Apply(const SelectionEvent& event) {
        const auto gesture = EnsureGesture();
        if (!gesture) {
            return std::unexpected(gesture.error());
        }
        GhosttySelection selection{};
        const auto result = ghostty_selection_gesture_event(
            *gesture, Terminal, event.get(), &selection);
        if (result == GHOSTTY_NO_VALUE) {
            return false;
        }
        if (result != GHOSTTY_SUCCESS) {
            return std::unexpected(Failed("cannot apply a gesture", result));
        }
        if (const auto set = ghostty_terminal_set(
                Terminal, GHOSTTY_TERMINAL_OPT_SELECTION, &selection);
            set != GHOSTTY_SUCCESS) {
            return std::unexpected(Failed("cannot select", set));
        }
        return true;
    }

    void Clear() {
        ghostty_terminal_set(Terminal, GHOSTTY_TERMINAL_OPT_SELECTION, nullptr);
    }

    static void WritePty(GhosttyTerminal /*terminal*/, void* userdata,
                         const uint8_t* data, size_t len) {
        // A copy, so the handler may replace itself while it runs.
        const ResponseHandler respond =
            static_cast<const State*>(userdata)->Respond;
        const QByteArrayView response(reinterpret_cast<const char*>(data),
                                      static_cast<qsizetype>(len));
        // An exception must not unwind through the library's frames.
        [&]() noexcept { respond(response); }();
    }

    GhosttyTerminal Terminal;
    GhosttySelectionGesture Gesture = nullptr;
    ResponseHandler Respond;
};

VtTerminal::VtTerminal(std::unique_ptr<State> state)
    : State_(std::move(state)) {}

VtTerminal::VtTerminal(VtTerminal&&) noexcept = default;
VtTerminal& VtTerminal::operator=(VtTerminal&&) noexcept = default;
VtTerminal::~VtTerminal() = default;

std::expected<VtTerminal, QString> VtTerminal::Create(quint16 columns,
                                                      quint16 rows) {
    if (auto size = CheckSize(columns, rows); !size) {
        return std::unexpected(size.error());
    }
    GhosttyTerminal terminal = nullptr;
    if (const auto result =
            ghostty_terminal_new(nullptr, &terminal, columns, rows);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot create a terminal", result));
    }
    auto state = std::make_unique<State>(terminal);
    // Before any input: what came earlier cannot be reconstructed.
    std::size_t maxBytes = MAX_CONTINUATION_BYTES;
    if (const auto result = ghostty_terminal_set(
            terminal, GHOSTTY_TERMINAL_OPT_CONTINUATION_MAX_BYTES, &maxBytes);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot track continuations", result));
    }
    return VtTerminal(std::move(state));
}

std::expected<VtTerminal, QString> VtTerminal::FromSnapshot(
    QByteArrayView snapshot) {
    GhosttySnapshotDecoder raw = nullptr;
    if (const auto result = ghostty_snapshot_decoder_new_buf(
            nullptr, &raw, Bytes(snapshot),
            static_cast<std::size_t>(snapshot.size()));
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot read the snapshot", result));
    }
    const std::unique_ptr<std::remove_pointer_t<GhosttySnapshotDecoder>,
                          decltype(&ghostty_snapshot_decoder_free)>
        decoder(raw, &ghostty_snapshot_decoder_free);

    // The same limit the encoder works to: anything longer is not ours.
    std::size_t maxBytes = MAX_CONTINUATION_BYTES;
    if (const auto result = ghostty_snapshot_decoder_set(
            decoder.get(), GHOSTTY_SNAPSHOT_DECODER_OPT_MAX_CONTINUATION_BYTES,
            &maxBytes);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot read the snapshot", result));
    }

    GhosttyTerminal terminal = nullptr;
    if (const auto result =
            ghostty_snapshot_decoder_decode(decoder.get(), &terminal);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot decode the snapshot", result));
    }
    auto state = std::make_unique<State>(terminal);
    std::size_t consumed = 0;
    if (const auto result = ghostty_snapshot_decoder_get(
            decoder.get(), GHOSTTY_SNAPSHOT_DECODER_DATA_SOURCE_OFFSET,
            &consumed);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot read the snapshot", result));
    }
    if (consumed != static_cast<std::size_t>(snapshot.size())) {
        return std::unexpected(
            QStringLiteral("%1 bytes follow the snapshot")
                .arg(static_cast<std::size_t>(snapshot.size()) - consumed));
    }
    return VtTerminal(std::move(state));
}

void* VtTerminal::NativeHandle() const { return State_->Terminal; }

void VtTerminal::SetResponseHandler(ResponseHandler handler) {
    State_->Respond = std::move(handler);
    // Without a callback the library ignores queries instead of answering.
    if (State_->Respond) {
        ghostty_terminal_set(State_->Terminal, GHOSTTY_TERMINAL_OPT_USERDATA,
                             State_.get());
        ghostty_terminal_set(State_->Terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                             reinterpret_cast<const void*>(&State::WritePty));
    } else {
        ghostty_terminal_set(State_->Terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                             nullptr);
    }
}

void VtTerminal::Write(QByteArrayView bytes) {
    ghostty_terminal_vt_write(State_->Terminal, Bytes(bytes),
                              static_cast<std::size_t>(bytes.size()));
}

std::expected<void, QString> VtTerminal::Resize(quint16 columns, quint16 rows) {
    if (auto size = CheckSize(columns, rows); !size) {
        return size;
    }
    // Cell pixels only matter for image protocols and pixel size reports,
    // which the daemon does not serve.
    if (const auto result =
            ghostty_terminal_resize(State_->Terminal, columns, rows, 0, 0);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot resize", result));
    }
    return {};
}

std::expected<QByteArray, QString> VtTerminal::Snapshot() const {
    uint8_t* data = nullptr;
    std::size_t size = 0;
    if (const auto result = ghostty_snapshot_encode_alloc(
            State_->Terminal, nullptr, &data, &size);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot take a snapshot", result));
    }
    QByteArray snapshot(reinterpret_cast<const char*>(data),
                        static_cast<qsizetype>(size));
    ghostty_free(nullptr, data, size);
    return snapshot;
}

std::expected<QByteArray, QString> VtTerminal::Dump() const {
    GhosttyFormatter raw = nullptr;
    if (const auto result =
            ghostty_formatter_terminal_new(nullptr, &raw, State_->Terminal);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot create a formatter", result));
    }
    const std::unique_ptr<std::remove_pointer_t<GhosttyFormatter>,
                          decltype(&ghostty_formatter_free)>
        formatter(raw, &ghostty_formatter_free);
    GhosttyFormatterFormat format = GHOSTTY_FORMATTER_FORMAT_VT;
    GhosttyFormatterExtras extras = GHOSTTY_FORMATTER_EXTRAS_ALL;
    bool trim = false;
    for (const auto& [option, value] :
         {std::pair<GhosttyFormatterOption, const void*>{
              GHOSTTY_FORMATTER_OPT_EMIT, &format},
          {GHOSTTY_FORMATTER_OPT_EXTRAS, &extras},
          {GHOSTTY_FORMATTER_OPT_TRIM, &trim}}) {
        if (const auto result =
                ghostty_formatter_set(formatter.get(), option, value);
            result != GHOSTTY_SUCCESS) {
            return std::unexpected(Failed("cannot set up a formatter", result));
        }
    }
    QByteArray dump;
    if (const auto result = ghostty_formatter_format(
            formatter.get(),
            GhosttyWriter{.write = AppendTo, .userdata = &dump});
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot dump the terminal", result));
    }
    return dump;
}

std::expected<VtTerminal::WheelModes, QString> VtTerminal::ReadWheelModes()
    const {
    WheelModes modes;
    if (const auto result = ghostty_terminal_get(
            State_->Terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING,
            &modes.MouseTracking);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot read the mouse mode", result));
    }
    GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
    if (const auto result = ghostty_terminal_get(
            State_->Terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot read the screen", result));
    }
    modes.AlternateScreen = screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE;
    GhosttyTerminalModeConfig scroll{
        .mode = GHOSTTY_MODE_MOUSE_ALTERNATE_SCROLL, .value = false};
    if (const auto result = ghostty_terminal_get(
            State_->Terminal, GHOSTTY_TERMINAL_DATA_MODE, &scroll);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(
            Failed("cannot read the alternate scroll mode", result));
    }
    modes.AlternateScroll = scroll.value;
    GhosttyTerminalModeConfig cursorKeys{.mode = GHOSTTY_MODE_CURSOR_KEYS,
                                         .value = false};
    if (const auto result = ghostty_terminal_get(
            State_->Terminal, GHOSTTY_TERMINAL_DATA_MODE, &cursorKeys);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(
            Failed("cannot read the cursor keys mode", result));
    }
    modes.CursorKeysApplication = cursorKeys.value;
    return modes;
}

std::expected<void, QString> VtTerminal::ScrollViewport(qint64 rows) {
    GhosttyTerminalScrollViewport behavior{};
    behavior.tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_DELTA;
    behavior.value.delta = static_cast<intptr_t>(rows);
    if (const auto result =
            ghostty_terminal_scroll_viewport(State_->Terminal, behavior);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot scroll the viewport", result));
    }
    return {};
}

std::expected<void, QString> VtTerminal::ScrollViewportToBottom() {
    GhosttyTerminalScrollViewport behavior{};
    behavior.tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_BOTTOM;
    if (const auto result =
            ghostty_terminal_scroll_viewport(State_->Terminal, behavior);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot scroll the viewport", result));
    }
    return {};
}

std::expected<bool, QString> VtTerminal::ViewportAtBottom() const {
    bool active = true;
    if (const auto result = ghostty_terminal_get(
            State_->Terminal, GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE, &active);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot read the viewport", result));
    }
    return active;
}

std::expected<void, QString> VtTerminal::SelectionPress(
    const SelectionPointer& pointer, const MouseGeometry& geometry,
    const SelectionClick& click) {
    const auto ref = State_->RefUnder(pointer, geometry);
    if (!ref) {
        return std::unexpected(ref.error());
    }
    auto event = NewEvent(GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_PRESS);
    if (!event) {
        return std::unexpected(event.error());
    }
    const GhosttySurfacePosition position{.x = pointer.X, .y = pointer.Y};
    const double distance = geometry.CellWidth;
    const std::uint64_t time = click.TimeNs;
    const std::uint64_t interval = click.RepeatIntervalNs;
    for (const auto& [option, value] :
         {std::pair<GhosttySelectionGestureEventOption, const void*>{
              GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &*ref},
          {GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &position},
          {GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REPEAT_DISTANCE, &distance},
          {GHOSTTY_SELECTION_GESTURE_EVENT_OPT_TIME_NS, &time},
          {GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REPEAT_INTERVAL_NS,
           &interval}}) {
        if (auto set = SetOption(*event, option, value); !set) {
            return set;
        }
    }
    const auto selected = State_->Apply(*event);
    if (!selected) {
        return std::unexpected(selected.error());
    }
    if (*selected) {
        return {};
    }
    // A single click that selects nothing clears the old selection, as in
    // Ghostty; a second or third click that finds no word or line leaves it.
    std::uint8_t count = 0;
    if (const auto result = ghostty_selection_gesture_get(
            State_->Gesture, State_->Terminal,
            GHOSTTY_SELECTION_GESTURE_DATA_CLICK_COUNT, &count);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot read the click count", result));
    }
    if (count == 1) {
        State_->Clear();
    }
    return {};
}

std::expected<void, QString> VtTerminal::SelectionDrag(
    const SelectionPointer& pointer, const MouseGeometry& geometry) {
    const auto ref = State_->RefUnder(pointer, geometry);
    if (!ref) {
        return std::unexpected(ref.error());
    }
    auto event = NewEvent(GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_DRAG);
    if (!event) {
        return std::unexpected(event.error());
    }
    if (auto set =
            SetOption(*event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &*ref);
        !set) {
        return set;
    }
    if (auto set = State_->SetGeometry(*event, pointer, geometry); !set) {
        return set;
    }
    // Not across a cell yet: nothing is selected, as in Ghostty.
    const auto selected = State_->Apply(*event);
    if (!selected) {
        return std::unexpected(selected.error());
    }
    if (!*selected) {
        State_->Clear();
    }
    return {};
}

std::expected<void, QString> VtTerminal::SelectionRelease(
    const SelectionPointer& pointer, const MouseGeometry& geometry) {
    const auto ref = State_->RefUnder(pointer, geometry);
    if (!ref) {
        return std::unexpected(ref.error());
    }
    auto event = NewEvent(GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_RELEASE);
    if (!event) {
        return std::unexpected(event.error());
    }
    if (auto set =
            SetOption(*event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &*ref);
        !set) {
        return set;
    }
    const auto applied = State_->Apply(*event);
    if (!applied) {
        return std::unexpected(applied.error());
    }
    return {};
}

std::expected<void, QString> VtTerminal::SelectionAutoscrollTick(
    const SelectionPointer& pointer, const MouseGeometry& geometry) {
    const auto cell = State_->CellUnder(pointer, geometry);
    if (!cell) {
        return std::unexpected(cell.error());
    }
    auto event = NewEvent(GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_AUTOSCROLL_TICK);
    if (!event) {
        return std::unexpected(event.error());
    }
    if (auto set = SetOption(
            *event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_VIEWPORT, &*cell);
        !set) {
        return set;
    }
    if (auto set = State_->SetGeometry(*event, pointer, geometry); !set) {
        return set;
    }
    // Without a direction the tick does nothing; SelectionAutoscroll tells
    // the caller to stop.
    const auto applied = State_->Apply(*event);
    if (!applied) {
        return std::unexpected(applied.error());
    }
    return {};
}

std::expected<VtTerminal::ESelectionAutoscroll, QString>
VtTerminal::SelectionAutoscroll() const {
    if (State_->Gesture == nullptr) {
        return ESelectionAutoscroll::None;
    }
    GhosttySelectionGestureAutoscroll direction =
        GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_NONE;
    if (const auto result = ghostty_selection_gesture_get(
            State_->Gesture, State_->Terminal,
            GHOSTTY_SELECTION_GESTURE_DATA_AUTOSCROLL, &direction);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot read the autoscroll", result));
    }
    switch (direction) {
        case GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_UP:
            return ESelectionAutoscroll::Up;
        case GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_DOWN:
            return ESelectionAutoscroll::Down;
        default:
            return ESelectionAutoscroll::None;
    }
}

std::expected<void, QString> VtTerminal::ResetSelectionGesture() {
    ghostty_selection_gesture_reset(State_->Gesture, State_->Terminal);
    return {};
}

std::expected<void, QString> VtTerminal::ClearSelection() {
    State_->Clear();
    return {};
}

std::expected<bool, QString> VtTerminal::HasSelection() const {
    GhosttySelection selection{};
    const auto result = ghostty_terminal_get(
        State_->Terminal, GHOSTTY_TERMINAL_DATA_SELECTION, &selection);
    if (result == GHOSTTY_NO_VALUE) {
        return false;
    }
    if (result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot read the selection", result));
    }
    return true;
}

std::expected<QString, QString> VtTerminal::SelectedText() const {
    GhosttySelection selection{};
    const auto got = ghostty_terminal_get(
        State_->Terminal, GHOSTTY_TERMINAL_DATA_SELECTION, &selection);
    if (got == GHOSTTY_NO_VALUE) {
        return QString();
    }
    if (got != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot read the selection", got));
    }
    GhosttyFormatter raw = nullptr;
    if (const auto result =
            ghostty_formatter_terminal_new(nullptr, &raw, State_->Terminal);
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot create a formatter", result));
    }
    const std::unique_ptr<std::remove_pointer_t<GhosttyFormatter>,
                          decltype(&ghostty_formatter_free)>
        formatter(raw, &ghostty_formatter_free);
    GhosttyFormatterFormat format = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    bool unwrap = true;
    bool trim = true;
    for (const auto& [option, value] :
         {std::pair<GhosttyFormatterOption, const void*>{
              GHOSTTY_FORMATTER_OPT_EMIT, &format},
          {GHOSTTY_FORMATTER_OPT_UNWRAP, &unwrap},
          {GHOSTTY_FORMATTER_OPT_TRIM, &trim},
          {GHOSTTY_FORMATTER_OPT_SELECTION, &selection}}) {
        if (const auto result =
                ghostty_formatter_set(formatter.get(), option, value);
            result != GHOSTTY_SUCCESS) {
            return std::unexpected(Failed("cannot set up a formatter", result));
        }
    }
    QByteArray text;
    if (const auto result = ghostty_formatter_format(
            formatter.get(),
            GhosttyWriter{.write = AppendTo, .userdata = &text});
        result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot copy the selection", result));
    }
    return QString::fromUtf8(text);
}

bool VtTerminal::BracketedPaste() const {
    GhosttyTerminalModeConfig mode{.mode = GHOSTTY_MODE_BRACKETED_PASTE,
                                   .value = false};
    return ghostty_terminal_get(State_->Terminal, GHOSTTY_TERMINAL_DATA_MODE,
                                &mode) == GHOSTTY_SUCCESS &&
           mode.value;
}

std::expected<QByteArray, QString> VtTerminal::EncodePaste(QStringView text,
                                                           bool bracketed) {
    // The library cleans the buffer in place.
    QByteArray data = text.toUtf8();
    std::size_t needed = 0;
    auto result =
        ghostty_paste_encode(data.data(), static_cast<std::size_t>(data.size()),
                             bracketed, nullptr, 0, &needed);
    if (result == GHOSTTY_SUCCESS) {
        return QByteArray();
    }
    if (result != GHOSTTY_OUT_OF_SPACE) {
        return std::unexpected(Failed("cannot encode the paste", result));
    }
    QByteArray encoded(static_cast<qsizetype>(needed), Qt::Uninitialized);
    std::size_t written = 0;
    result = ghostty_paste_encode(
        data.data(), static_cast<std::size_t>(data.size()), bracketed,
        reinterpret_cast<uint8_t*>(encoded.data()), needed, &written);
    if (result != GHOSTTY_SUCCESS) {
        return std::unexpected(Failed("cannot encode the paste", result));
    }
    encoded.truncate(static_cast<qsizetype>(written));
    return encoded;
}

}  // namespace roost
