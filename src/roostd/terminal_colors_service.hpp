#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <optional>

#include "core/store.hpp"
#include "core/terminal_theme.hpp"
#include "roostd/server.hpp"

namespace roost {

class WarmPool;

// Keeps the colours of the daemon's terminals as the theme of the `settings`
// document has them (design.md §5, §8), so that what a program in a session
// is told when it asks for the colours (OSC 10, 11, 12, 4, CSI ? 996 n) is
// what the window draws.
//
// It reads settings.theme at start and again after every store.set and
// store.patch of the document, by the store.changed event those publish; the
// colours go to all warm terminals and to those opened later (WarmPool). A
// file edited by hand is not noticed. A theme the daemon does not know, or a
// document that cannot be read, gives the default theme, and a line in the
// log says so once.
//
// The server and the pool must outlive it.
class TerminalColorsService : public QObject {
    Q_OBJECT

public:
    TerminalColorsService(Store store, Server& server, WarmPool& pool,
                          QObject* parent = nullptr);
    ~TerminalColorsService() override;

    TerminalColorsService(const TerminalColorsService&) = delete;
    TerminalColorsService& operator=(const TerminalColorsService&) = delete;

private:
    void Apply(const QJsonObject& settings);

    Server& Server_;
    WarmPool& Pool_;
    EventBus::SubscriptionId Subscription_ = 0;
    // What the pool has, and the problem last logged.
    std::optional<TerminalColors> Applied_;
    QString Logged_;
};

}  // namespace roost
