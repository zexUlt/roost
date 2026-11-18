#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QtTypes>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "terminal/io_thread.hpp"
#include "terminal/terminal_session.hpp"

namespace roost {

// A session of an agent, as roostd names it.
struct SessionKey {
    QString Agent;
    QString Id;

    bool operator==(const SessionKey&) const = default;
};

// Where a session stands in the window.
enum class EPaneStatus {
    // Nothing to show.
    None,
    // session.open is in flight; there is no screen yet.
    Opening,
    // The channel is live.
    Open,
    // The session ended by itself; its last screen stays.
    Ended,
    // The open was refused.
    Failed,
    // The connection to roostd was lost; the last screen stays.
    Disconnected,
};

// The window's warm terminals (design.md §3): the channel, the terminal and
// the state of each session the window has opened, kept when no panel shows
// it, so that showing it again is a switch of one frame and not an open.
//
// An entry per SessionKey is Shown while a panel (SessionPane) has Acquired
// it, and hidden after the Release. Release does not close the channel.
// roostd decides what stays warm: it closes the channel of a session it
// evicts, and the window drops the entry when session.closed arrives for it.
// The window has no limit of its own and so cannot drift from the daemon's.
// Acquire of a warm session tells roostd it was viewed (session.view), which
// is what keeps its LRU in step with the switches made in the window.
//
// A hidden entry is never acknowledged (IoThread::AcknowledgeUpdate), so
// its channel raises at most one Updated and then costs the GUI nothing; the
// IO thread keeps feeding its terminal, and the screen is current when it is
// shown. Its PTY keeps the size it last had: the resize it needs is sent
// once, when it is shown again.
//
// What becomes of an entry:
//   - hidden, the channel closes (evicted, exited, removed): dropped;
//   - shown, the channel closes: Ended, the last screen kept;
//   - the open fails: dropped if hidden, Failed if shown;
//   - the connection is lost: every hidden entry is dropped, a shown one is
//     Disconnected with its screen kept, and ReopenShown opens it again once
//     the window is back (Prefetch calls it, after the hidden opens);
//   - an open still in flight when the session is hidden completes into a
//     hidden warm entry, as roostd holds the session either way;
//   - an entry that is Release'd while Ended, Failed or Disconnected is
//     dropped;
//   - Prefetch opens the sessions roostd keeps warm as hidden entries, so
//     that the first show after a restart of the window is warm as well. It
//     opens them without a size, so as not to resize what a panel may be
//     showing elsewhere: the one resize is sent when the entry is first shown.
//     The shown sessions are opened after them, so that roostd touches them
//     last and the session on screen is its most recent one.
//
// At most one owner shows a key (a session has one render state): the entry
// records who Acquired it. A second Acquire of a shown key from another owner
// takes the show over and says Superseded, so that the old owner stops
// showing it; the old owner's Release then changes nothing.
//
// Uses an IoThread and lives in the thread of it (the GUI thread). The
// IoThread must outlive the pool. The destructor closes every channel.
class WarmTerminals : public QObject {
    Q_OBJECT

public:
    explicit WarmTerminals(IoThread& io, QObject* parent = nullptr);
    ~WarmTerminals() override;

    WarmTerminals(const WarmTerminals&) = delete;
    WarmTerminals& operator=(const WarmTerminals&) = delete;

    // Shows the session in the panel: a warm one at once (its terminal and
    // the status Open, with no Opening in between, then the acknowledgement,
    // session.view and a resize if the size differs), any other one with a
    // session.open at the grid. An open in flight is waited for. A shown
    // session goes to the new owner, with what a warm show does if it is
    // Open. An Ended, Failed or Disconnected session is not opened here: see
    // Reopen. The owner is only compared, never used.
    void Acquire(const SessionKey& key, const QObject* owner, quint16 columns,
                 quint16 rows);
    // The panel no longer shows the session. A live or opening one stays
    // warm; any other is dropped. Does nothing unless the owner is the one
    // that shows it.
    void Release(const SessionKey& key, const QObject* owner);

    // The grid wanted for the session. Only the latest size is sent, once the
    // session is shown: at most one session.resize is in flight, and the
    // newest wish waits for it.
    void SetGrid(const SessionKey& key, quint16 columns, quint16 rows);
    // Input for the program; dropped unless the session is Open.
    void SendInput(const SessionKey& key, const QByteArray& bytes);
    // Opens a shown session again if it is Ended, Failed or Disconnected; the
    // screen clears while it opens. Otherwise does nothing.
    void Reopen(const SessionKey& key);
    // Reopen of every shown session that is Disconnected. Hidden sessions are
    // not opened. Prefetch calls it, so the window needs only Prefetch when
    // it is connected again.
    void ReopenShown();
    // For when the window is connected: opens, hidden, the sessions roostd
    // keeps warm that have no entry yet, then does ReopenShown. The hidden
    // ones are opened oldest first, so that roostd's order of recent use is
    // the same after as before, and the shown ones after them, so that they
    // are the most recent. An error answer to sessions.warm opens no hidden
    // session, and the shown ones are reopened all the same.
    void Prefetch();

    // None, and a null terminal, for a session the pool has no entry for.
    EPaneStatus Status(const SessionKey& key) const;
    // Why the status is what it is: roostd's reason for Ended and Failed, the
    // loss for Disconnected; empty otherwise.
    QString Message(const SessionKey& key) const;
    // Null while Opening, Failed and for an unknown key; kept in Ended and
    // Disconnected.
    std::shared_ptr<TerminalSession> Session(const SessionKey& key) const;
    // The live channel, 0 if there is none.
    std::uint32_t Channel(const SessionKey& key) const;
    // Whether a panel shows the session.
    bool IsShown(const SessionKey& key) const;
    // Who shows the session; null if nobody or no entry.
    const QObject* Owner(const SessionKey& key) const;
    // The sessions the pool has an entry for.
    std::size_t Size() const { return Entries_.size(); }

signals:
    // Session(key) is another terminal, or null.
    void SessionChanged(const roost::SessionKey& key);
    // The terminal of a shown session has new content to draw.
    void Updated(const roost::SessionKey& key);
    // Status(key) or Message(key) changed.
    void StatusChanged(const roost::SessionKey& key);
    // Another owner has Acquired the shown session; Owner(key) is it.
    void Superseded(const roost::SessionKey& key);

private:
    struct Entry {
        SessionKey Key;
        // 0 while there is no live channel.
        std::uint32_t Channel = 0;
        std::shared_ptr<TerminalSession> Session;
        EPaneStatus Status = EPaneStatus::None;
        QString Message;
        // Of the open in flight, or the last one; an answer of another one is
        // closed at once and ignored.
        std::uint64_t Generation = 0;
        bool Shown = false;
        // Who Acquired it; null while hidden.
        const QObject* Owner = nullptr;
        std::pair<quint16, quint16> Grid{80, 24};
        // The size the live channel was last given, by the open or a resize.
        std::optional<std::pair<quint16, quint16>> SentSize;
        bool ResizeInFlight = false;
    };

    Entry* Find(const SessionKey& key) const;
    Entry* FindByChannel(std::uint32_t channel) const;
    void Erase(const Entry* entry);

    // Opens the entry at its grid, or without a size if not Sized (then
    // SentSize stays empty, and the first show sends the resize); the screen
    // of an earlier open goes. Emits, so it works from the key and stops if
    // the entry is gone or opened again.
    void StartOpen(const SessionKey& key, bool sized = true);
    void OnOpened(const SessionKey& key, std::uint64_t generation,
                  const OpenResult& result);
    void OnUpdated(std::uint32_t channel);
    void OnClosed(std::uint32_t channel, const QString& reason);
    void OnDisconnected(const QString& reason);
    // Sends session.view for a shown entry's channel.
    void SendView(std::uint32_t channel);
    // Sends the latest wanted size of a shown entry if none is in flight.
    void PumpResize(Entry& entry);
    void SetStatus(Entry& entry, EPaneStatus status,
                   const QString& message = {});

    IoThread& Io_;
    // Few (roostd's pool, plus what is in flight): a list. Each is its own
    // allocation, so a pointer to an entry stays valid while others come and
    // go.
    std::vector<std::unique_ptr<Entry>> Entries_;
    std::uint64_t LastGeneration_ = 0;
};

}  // namespace roost
