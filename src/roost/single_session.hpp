#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QtTypes>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "qml_api/terminal_item.hpp"
#include "terminal/io_thread.hpp"
#include "terminal/terminal_session.hpp"

namespace roost {

// Developer mode (design.md §13 M3): one TerminalItem wired to one session of
// roostd, with no plugins in between. It is the whole of what a pane does:
// open the session at the item's grid, show it, send the item's input to the
// PTY, follow the item's size, and redraw when output arrives.
//
// It is a plain C++ object and not a QML type, since the window creates it
// from main(); the item it drives is the QML-visible part.
//
// Lives in the GUI thread. Ended or Failed is emitted at most once, and
// after it the controller does nothing more.
class SingleSession : public QObject {
    Q_OBJECT

public:
    // The item belongs to the QML scene; the controller tolerates its
    // deletion.
    SingleSession(TerminalItem* item, QString agent, QString session,
                  QObject* parent = nullptr);
    ~SingleSession() override;

    // Connects to the socket and opens the session at the item's current
    // grid. Call once.
    void Start(const QString& socketPath);

    // Null until Opened.
    std::shared_ptr<TerminalSession> Session() const;

signals:
    // The session is open and shown.
    void Opened();
    // The session ended by itself; Reason is roostd's ("exited with code N").
    void Ended(const QString& reason);
    // Something broke: the connection, the open or the daemon. Already
    // logged with qCritical.
    void Failed(const QString& message);

private:
    void OnOpened(const OpenResult& result);
    void OnUpdated(std::uint32_t channel);
    void OnClosed(std::uint32_t channel, const QString& reason);
    // Sends the latest wanted size if none is in flight.
    void PumpResize();
    void Fail(const QString& message);
    bool Finished() const { return Done_; }

    QPointer<TerminalItem> Item_;
    QString Agent_;
    QString SessionId_;
    IoThread Io_;
    OpenedSession Opened_;
    // Only the latest size matters: a window drag produces one per pixel, so
    // at most one resize is in flight and the newest wish waits for it.
    std::optional<std::pair<quint16, quint16>> WantedSize_;
    std::optional<std::pair<quint16, quint16>> SentSize_;
    bool ResizeInFlight_ = false;
    bool Done_ = false;
};

}  // namespace roost
