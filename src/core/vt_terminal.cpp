#include "core/vt_terminal.hpp"

#include "core/ghostty_result.hpp"

#include <ghostty/vt.h>

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

}  // namespace

struct VtTerminal::State {
    explicit State(GhosttyTerminal terminal) : Terminal(terminal) {}
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State() { ghostty_terminal_free(Terminal); }

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

}  // namespace roost
