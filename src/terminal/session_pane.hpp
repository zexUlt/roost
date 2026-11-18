#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QtTypes>

#include <memory>
#include <optional>
#include <utility>

#include "terminal/terminal_session.hpp"
#include "terminal/warm_terminals.hpp"

namespace roost {

// A view of the window's warm terminals (warm_terminals.hpp) for one panel
// (design.md §3): which session it shows, the size it asks for, and what
// became of it. It holds no item: whoever draws the session listens to the
// signals and calls Session().
//
// Show switches the session: it releases the old one, which stays warm in
// WarmTerminals with its channel open, and acquires the new one. A warm
// session is shown at once, with its terminal and the status Open and no
// Opening in between; any other one clears the screen (Session() is null) and
// is Opening until roostd's answer. Switching never closes a channel, and
// neither does the destructor or Show(nullopt): roostd decides what stays
// warm, and the pool closes the channels it holds when it is destroyed.
//
// The channel is silent after Open until the first AcknowledgeUpdate; the pool
// does it before the pane says Updated for the first time, and again for each
// Updated it passes on.
//
// Lives in the thread of the pool (the GUI thread). The pool must outlive the
// pane. At most one pane shows a session: when another pane Shows it, the
// newest takes it over, and this one presents no session, the status None, and
// sends nothing, until it Shows something else.
class SessionPane : public QObject {
    Q_OBJECT

public:
    explicit SessionPane(WarmTerminals& terminals, QObject* parent = nullptr);
    // Releases the session; its channel stays open in the pool.
    ~SessionPane() override;

    // Shows the session, or nothing for nullopt. The same session again does
    // nothing while it is Opening or Open, and reopens it from Ended, Failed
    // or Disconnected.
    void Show(const std::optional<SessionKey>& key);

    // The grid the session is opened at and kept at. Only the latest size is
    // sent: at most one session.resize is in flight, and the newest wish
    // waits for it.
    void SetGrid(quint16 columns, quint16 rows);

    // Input for the program; dropped unless Open.
    void SendInput(const QByteArray& bytes);

    // Opens the shown session again, if it is Ended, Failed or Disconnected;
    // the screen clears while it opens. Otherwise does nothing.
    void Reopen();

    // The terminal to draw. Null while Opening, Failed and None; kept in
    // Ended and Disconnected.
    std::shared_ptr<TerminalSession> Session() const;
    EPaneStatus Status() const;
    // Why the status is what it is: roostd's reason for Ended and Failed, the
    // loss for Disconnected; empty otherwise.
    QString Message() const;
    const std::optional<SessionKey>& Key() const { return Key_; }

signals:
    // Session() is another terminal, or null.
    void SessionChanged();
    // The terminal has new content to draw.
    void Updated();
    // Status() or Message() changed.
    void StatusChanged();

private:
    // Whether the pool has this pane as the one showing its key.
    bool Owns() const;
    // Says SessionChanged or StatusChanged if the pool has moved on from what
    // the pane last said.
    void SyncSession();
    void SyncStatus();

    WarmTerminals& Terminals_;
    std::optional<SessionKey> Key_;
    std::pair<quint16, quint16> Grid_{80, 24};
    // What the pane last said.
    std::shared_ptr<TerminalSession> Announced_;
    EPaneStatus AnnouncedStatus_ = EPaneStatus::None;
    QString AnnouncedMessage_;
};

}  // namespace roost
