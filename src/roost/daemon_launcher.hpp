#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <functional>

#include "api/api.hpp"

namespace roost {

// Starts roostd when the window finds nobody at the socket (design.md §4,
// "Запуск демона окном"). Plain C++, made from main() like Shell and
// SingleSession; GUI thread only.
//
// The trigger is Api::NoListener: no socket file, or a file nobody answers on.
// A server that answers and refuses (Api::Refused) is not one, and a roostd
// that is up is never started over: a second one would lose the socket lock
// and exit anyway.
//
// One start is in flight at a time. It begins with the start of the process
// and ends when the Api connects or is refused, or after `StartTimeout`
// (roostd died while starting, or never came up): the Api's retry keeps
// reporting NoListener while the new roostd comes up, and none of those starts
// another. A NoListener after the end starts again, which is how a roostd that
// died later comes back.
class DaemonLauncher : public QObject {
    Q_OBJECT

public:
    struct Command {
        QString Program;
        QStringList Arguments;
    };

    // Starts the process; false if it did not start. The default detaches it
    // (StartDetached). A test replaces it to count the starts.
    using StartFunction = std::function<bool(const Command&)>;

    static constexpr std::chrono::milliseconds DEFAULT_START_TIMEOUT{
        std::chrono::seconds(15)};

    // The roostd of this installation: `roostd` next to the executable, or
    // `$APPIMAGE --roostd` inside an AppImage; `--log <dir of the
    // socket>/roostd.log` either way.
    static Command DefaultCommand(const QString& socketPath);

    // Starts the process alone in its own session, with no terminal and no
    // inherited descriptors, standard streams on /dev/null, in $HOME, so that
    // it outlives the window. False if it did not start.
    static bool StartDetached(const Command& command);

    // `api` is listened to, not owned.
    DaemonLauncher(
        api::Api& api, Command command,
        std::chrono::milliseconds startTimeout = DEFAULT_START_TIMEOUT,
        StartFunction start = &DaemonLauncher::StartDetached,
        QObject* parent = nullptr);

    // Whether a start is in flight; the window shows "Starting roostd…".
    bool Starting() const { return Starting_; }

signals:
    void StartingChanged(bool starting);

private:
    void OnNoListener();
    void SetStarting(bool starting);

    Command Command_;
    StartFunction Start_;
    QTimer Timeout_;
    bool Starting_ = false;
};

}  // namespace roost
