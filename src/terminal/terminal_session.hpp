#pragma once

#include "core/vt_render_state.hpp"
#include "core/vt_terminal.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

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
};

}  // namespace roost
