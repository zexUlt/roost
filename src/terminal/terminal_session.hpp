#pragma once

#include "core/vt_key_encoder.hpp"
#include "core/vt_mouse_encoder.hpp"
#include "core/vt_render_state.hpp"
#include "core/vt_terminal.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringView>

#include <atomic>
#include <expected>
#include <memory>
#include <mutex>

namespace roost {

// The window's owner of one terminal: the terminal and the mutex that
// serializes everything done to it (design.md §3). libghostty-vt is not
// thread-safe, and TSan cannot see inside it, so this is the lock check the
// design asks for.
//
// Every method that touches the terminal takes a `Locked` token, which only
// Lock() hands out and which holds the mutex until it is destroyed. Calling
// such a method without the lock does not compile. In a debug build each
// method also checks that the token is of this very session, which catches
// what the types cannot: a token of another session. A failed check calls the
// check handler, which by default is fatal; release builds do not check.
//
// The session also owns the one VtRenderState of its terminal: beginning an
// update consumes the terminal's dirty state, so a second state would miss
// the changes the first took. Exactly one thread renders a session. Several
// panes on one session must share its single dirty state, which a later
// milestone handles.
//
// Threads: the IO thread writes under the lock; the GUI thread takes it
// briefly to sync; the render thread begins an update under the lock, drops
// it, and finishes and reads the render state without it.
//
// A session cannot be copied or moved: tokens point at it.
class TerminalSession {
public:
    // Proof that the session's mutex is held. Not copyable, not movable:
    // keep it in a variable, and let it go out of scope to unlock.
    class Locked {
    public:
        Locked(const Locked&) = delete;
        Locked& operator=(const Locked&) = delete;
        ~Locked() = default;

    private:
        friend class TerminalSession;
        explicit Locked(TerminalSession& session);

        // Read only by the debug-build check.
        [[maybe_unused]] TerminalSession* Session_;
        std::unique_lock<std::mutex> Lock_;
    };

    // Called with a description when a debug-build check fails. Must not
    // return normally unless a test replaced it: after it returns the method
    // does nothing and reports failure.
    using CheckHandler = void (*)(const char* message);
    // Replaces the handler and returns the previous one; null restores the
    // default, qFatal. For tests: not thread-safe against running checks.
    static CheckHandler SetCheckHandler(CheckHandler handler);

    static std::expected<std::unique_ptr<TerminalSession>, QString> Create(
        quint16 columns, quint16 rows);
    // See VtTerminal::FromSnapshot for what the snapshot may be.
    static std::expected<std::unique_ptr<TerminalSession>, QString>
    FromSnapshot(QByteArrayView snapshot);

    // Public only for std::make_unique: Key can be made by nobody else, so use
    // Create or FromSnapshot.
    class Key {
        friend class TerminalSession;
        Key() = default;
    };
    TerminalSession(Key key, VtTerminal terminal);

    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;
    ~TerminalSession();

    // Blocks until the mutex is free. Do not call it while holding a token
    // of this session: the mutex is not recursive.
    Locked Lock();

    void Write(const Locked& lock, QByteArrayView bytes);
    std::expected<void, QString> Resize(const Locked& lock, quint16 columns,
                                        quint16 rows);
    // For tests: see VtTerminal::Dump.
    std::expected<QByteArray, QString> Dump(const Locked& lock) const;
    // Copies the terminal's input modes (cursor keys, keypad, Kitty keyboard
    // protocol, ...) into the encoder. The GUI thread does it under a short
    // lock before encoding a key; the encoding itself then needs no lock.
    std::expected<void, QString> SyncKeyEncoder(const Locked& lock,
                                                VtKeyEncoder& encoder) const;
    // Copies the terminal's mouse tracking mode and format into the encoder,
    // with the geometry of the grid on screen. As for keys: a short lock for
    // the sync, no lock for the encoding.
    std::expected<void, QString> SyncMouseEncoder(
        const Locked& lock, VtMouseEncoder& encoder,
        const MouseGeometry& geometry) const;
    // What the wheel should do now; see VtTerminal::ReadWheelModes.
    std::expected<VtTerminal::WheelModes, QString> ReadWheelModes(
        const Locked& lock) const;
    // Moves the viewport by whole rows, negative toward older output; see
    // VtTerminal::ScrollViewport. The render state picks the move up at the
    // next update, so the caller asks for a frame.
    std::expected<void, QString> ScrollViewport(const Locked& lock,
                                                qint64 rows);
    // Back to the active area: what typing and pasting do.
    std::expected<void, QString> ScrollViewportToBottom(const Locked& lock);
    // False while the viewport is scrolled into history.
    std::expected<bool, QString> ViewportAtBottom(const Locked& lock) const;
    // The selection, kept in the terminal; see VtTerminal for the gestures.
    // The viewport and the selection change, so the caller asks for a frame
    // after each of these.
    std::expected<void, QString> SelectionPress(
        const Locked& lock, const VtTerminal::SelectionPointer& pointer,
        const MouseGeometry& geometry, const VtTerminal::SelectionClick& click);
    std::expected<void, QString> SelectionDrag(
        const Locked& lock, const VtTerminal::SelectionPointer& pointer,
        const MouseGeometry& geometry);
    std::expected<void, QString> SelectionRelease(
        const Locked& lock, const VtTerminal::SelectionPointer& pointer,
        const MouseGeometry& geometry);
    std::expected<void, QString> SelectionAutoscrollTick(
        const Locked& lock, const VtTerminal::SelectionPointer& pointer,
        const MouseGeometry& geometry);
    std::expected<VtTerminal::ESelectionAutoscroll, QString>
    SelectionAutoscroll(const Locked& lock) const;
    std::expected<void, QString> ResetSelectionGesture(const Locked& lock);
    std::expected<void, QString> ClearSelection(const Locked& lock);
    std::expected<bool, QString> HasSelection(const Locked& lock) const;
    // The selected text, as plain text for the clipboard; see
    // VtTerminal::SelectedText. Linear in the size of the selection, under
    // the lock.
    std::expected<QString, QString> SelectedText(const Locked& lock) const;
    // Whether the terminal had a selection at the last BeginRenderUpdate.
    // The terminal drops it by itself when its text is erased, which the GUI
    // thread learns of from this. Readable from any thread.
    bool SelectionAtLastRender() const { return SelectionAtLastRender_; }
    // Whether bracketed paste is on, for VtTerminal::EncodePaste, which then
    // runs without the lock. False for a wrong token.
    bool BracketedPaste(const Locked& lock) const;
    // The first phase of a render update, for the render thread: the state
    // may be finished and read after the token is gone.
    std::expected<void, QString> BeginRenderUpdate(const Locked& lock);

    // The rest is for the render thread only and needs no lock: the render
    // state does not touch the terminal outside BeginRenderUpdate.
    //
    // Finishes the update; see VtRenderState::EndUpdate.
    void EndRenderUpdate();
    // Rows, cursor, dirty state and colors, valid after EndRenderUpdate.
    const VtRenderState& RenderState() const;
    // Marks the frame drawn; see VtRenderState::Clean.
    void CleanRenderState();

private:
    // True when the token is this session's; otherwise calls the check
    // handler and returns false. Always true in release.
    bool Check(const Locked& lock, const char* method) const;

    VtTerminal Terminal_;
    // Begun under Mutex_ and used by the render thread alone otherwise.
    VtRenderState RenderState_;
    std::mutex Mutex_;
    std::atomic<bool> SelectionAtLastRender_ = false;
};

}  // namespace roost
