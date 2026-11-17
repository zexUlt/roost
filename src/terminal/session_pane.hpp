#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QtTypes>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "terminal/io_thread.hpp"
#include "terminal/terminal_session.hpp"

namespace roost {

// A session of an agent, as roostd names it.
struct SessionKey {
    QString Agent;
    QString Id;

    bool operator==(const SessionKey&) const = default;
};

// Where a pane stands with its session.
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

// The channel logic of one terminal panel (design.md §3): which session it
// shows, the channel that carries it, the size it asks for, and what became
// of it. It is what SingleSession does for one fixed session, reusable for a
// panel whose session changes. It holds no item: whoever draws the session
// listens to the signals and calls Session().
//
// Show switches the session. The old channel is closed (not waited for), the
// screen clears (Session() is null) and the status is Opening until
// roostd's answer. Each open carries a generation; the answer of an older
// one, such as the middle of a quick A, B, C, is closed at once and ignored.
// If the pane is gone when an answer comes, its channel is closed too.
//
// The channel is silent after Open until the first AcknowledgeUpdate; the pane
// does it before it says Updated for the first time, and again for each
// Updated of the channel it passes on.
//
// Uses an IoThread and lives in the thread of it (the GUI thread). The
// IoThread must outlive the pane.
class SessionPane : public QObject {
    Q_OBJECT

public:
    explicit SessionPane(IoThread& io, QObject* parent = nullptr);
    // Closes the channel.
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
    std::shared_ptr<TerminalSession> Session() const { return Session_; }
    EPaneStatus Status() const { return Status_; }
    // Why the status is what it is: roostd's reason for Ended and Failed, the
    // loss for Disconnected; empty otherwise.
    const QString& Message() const { return Message_; }
    const std::optional<SessionKey>& Key() const { return Key_; }

signals:
    // Session() is another terminal, or null.
    void SessionChanged();
    // The terminal has new content to draw.
    void Updated();
    // Status() or Message() changed.
    void StatusChanged();

private:
    // Closes the old channel and opens Key_ at the current grid.
    void StartOpen();
    void OnOpened(std::uint64_t generation, const OpenResult& result);
    void OnUpdated(std::uint32_t channel);
    void OnClosed(std::uint32_t channel, const QString& reason);
    void OnDisconnected(const QString& reason);
    // Sends the latest wanted size if none is in flight.
    void PumpResize();
    // session.close for the channel, if there is one; the answer is not
    // awaited.
    void CloseChannel();
    void SetStatus(EPaneStatus status, const QString& message = {});

    IoThread& Io_;
    std::optional<SessionKey> Key_;
    std::shared_ptr<TerminalSession> Session_;
    // 0 while there is no live channel.
    std::uint32_t Channel_ = 0;
    std::uint64_t Generation_ = 0;
    EPaneStatus Status_ = EPaneStatus::None;
    QString Message_;
    std::pair<quint16, quint16> Grid_{80, 24};
    // The size the live channel was last given, by the open or a resize.
    std::optional<std::pair<quint16, quint16>> SentSize_;
    bool ResizeInFlight_ = false;
};

}  // namespace roost
