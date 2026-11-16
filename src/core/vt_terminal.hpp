#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringView>

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>

namespace roost {

// A terminal of libghostty-vt that a session's output is fed into
// (design.md §3): the daemon keeps one per warm session and hands its snapshot
// to the window, which goes on feeding the restored copy.
//
// The stream may be cut anywhere, inside an escape sequence or a UTF-8
// character: the terminal remembers the unfinished part, the snapshot carries
// it, and the restored terminal finishes it with the next bytes. A sequence
// left unfinished for longer than MAX_CONTINUATION_BYTES cannot be
// snapshotted until the stream gets past it.
//
// Queries in the stream (DA, CPR, ...) are answered only when a response
// handler is set: the window's copy must stay silent, or the program would
// get two answers to one query.
class VtTerminal {
public:
    // Gets the bytes to write back to the program. Called from inside Write,
    // and from inside Resize for a size report the program asked for. It may
    // replace or clear the handler, but must not write to, resize or destroy
    // the terminal. It must not throw: an exception ends the process.
    using ResponseHandler = std::function<void(QByteArrayView response)>;

    static constexpr std::size_t MAX_CONTINUATION_BYTES = 1 << 20;
    // Columns and rows each; a cell costs memory whether it is shown or not.
    static constexpr quint16 MAX_SIZE = 2000;

    static std::expected<VtTerminal, QString> Create(quint16 columns,
                                                     quint16 rows);
    // A terminal in the state the snapshot was taken in, without a response
    // handler. It finishes the sequence the snapshot was cut in, but cannot
    // itself be snapshotted while inside one. Bytes after the snapshot are
    // refused. The snapshot must come from a roostd of the user's:
    // libghostty-vt is not hardened against a crafted one (design.md §3).
    static std::expected<VtTerminal, QString> FromSnapshot(
        QByteArrayView snapshot);

    // A moved-from terminal may only be destroyed or assigned to.
    VtTerminal(VtTerminal&&) noexcept;
    VtTerminal& operator=(VtTerminal&&) noexcept;
    ~VtTerminal();

    void SetResponseHandler(ResponseHandler handler);
    void Write(QByteArrayView bytes);
    // Both sizes must be from 1 to MAX_SIZE, as for Create.
    std::expected<void, QString> Resize(quint16 columns, quint16 rows);
    std::expected<QByteArray, QString> Snapshot() const;
    // VT sequences that recreate the active screen with its scrollback, as
    // far as libghostty-vt can say it: text, styles, hyperlinks, modes, the
    // cursor, scrolling region, tab stops, character sets. The title, colors
    // set by OSC and the saved cursor are left out, though a snapshot keeps
    // them. Snapshots of terminals with equal dumps may still differ in bytes,
    // since those carry ids the library assigns.
    std::expected<QByteArray, QString> Dump() const;
    // Whether the program turned bracketed paste (mode 2004) on; false too
    // when the library cannot say.
    bool BracketedPaste() const;
    // What to write to the program for a paste of this text: wrapped in
    // ESC[200~ ... ESC[201~ when `bracketed`; with newlines turned into
    // carriage returns when not. It needs no terminal, so the caller may do it
    // outside the session's lock, after reading BracketedPaste under it.
    // Control bytes that could end the bracket or inject commands (ESC, NUL,
    // DEL, ...) become spaces either way, so the text cannot break out. It is
    // not ghostty_terminal_paste: that writes through the terminal's response
    // handler, which the window's copy must not have (it would answer queries
    // twice), and it refuses unsafe text instead of cleaning it.
    static std::expected<QByteArray, QString> EncodePaste(QStringView text,
                                                          bool bracketed);

    // The modes that decide what the mouse wheel does (design.md §3).
    struct WheelModes {
        // Some mouse tracking mode (X10, normal, button, any) is on.
        bool MouseTracking = false;
        // The alternate screen is the active one; it has no scrollback.
        bool AlternateScreen = false;
        // Alternate scroll (1007): on the alternate screen the wheel types
        // cursor keys.
        bool AlternateScroll = false;
        // DECCKM: the arrows are sent as SS3 (ESC O A), not CSI (ESC [ A).
        bool CursorKeysApplication = false;
    };
    std::expected<WheelModes, QString> ReadWheelModes() const;
    // Moves the viewport through the scrollback by whole rows: negative
    // toward older output, positive toward newer. Stops at the top and at
    // the active area. The viewport stays where it is while output arrives,
    // until it is moved to the bottom or the screen is cleared.
    std::expected<void, QString> ScrollViewport(qint64 rows);
    // Back to the active area, where the cursor is.
    std::expected<void, QString> ScrollViewportToBottom();
    // Whether the viewport follows the active area (not scrolled into
    // history).
    std::expected<bool, QString> ViewportAtBottom() const;

private:
    friend class VtKeyEncoder;
    friend class VtMouseEncoder;
    friend class VtRenderState;
    struct State;

    explicit VtTerminal(std::unique_ptr<State> state);
    // The libghostty-vt handle (a GhosttyTerminal), for VtRenderState and
    // VtKeyEncoder only,
    // so that no library type shows in this header.
    void* NativeHandle() const;

    std::unique_ptr<State> State_;
};

}  // namespace roost
