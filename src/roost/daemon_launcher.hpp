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
// A server that answers and refuses (Api::Refused) is not one. A roostd that
// is up is not started over either, except when its accept backlog is full
// and the attempt is refused: the second one loses the socket lock and exits.
//
// One start is in flight at a time. It begins with the start of the process
// and ends when the Api connects or is refused, or after `StartTimeout`
// (roostd died while starting, or never came up): the Api's retry keeps
// reporting NoListener while the new roostd comes up, and none of those starts
// another. A NoListener after the end starts again, which is how a roostd that
// died later comes back.
//
// A start that fails (the process does not start, or the timeout passes
// without a connection) leaves a Failure() text until roostd answers
// (Connected or Refused); later starts keep it, the window shows it in the
// banner.
class DaemonLauncher : public QObject {
    Q_OBJECT

public:
    struct Command {
        QString Program;
        QStringList Arguments;
        // Where the output of the process goes before it holds the socket lock
        // (after it roostd writes its own --log): appended to this file if its
        // directory exists, else /dev/null. Empty: /dev/null.
        QString StartLog;
    };

    // Starts the process; false, with `error` set, if it did not start. The
    // default detaches it (StartDetached). A test replaces it to count the
    // starts.
    using StartFunction = std::function<bool(const Command&, QString& error)>;

    static constexpr std::chrono::milliseconds DEFAULT_START_TIMEOUT{
        std::chrono::seconds(15)};

    // The roostd of this installation: `roostd` next to the executable, or
    // `$APPIMAGE --roostd` inside an AppImage (APPIMAGE and APPDIR set, and
    // the executable inside APPDIR); `--log <dir of the socket>/roostd.log`
    // and StartLog `<dir of the socket>/roostd.start.log` either way.
    static Command DefaultCommand(const QString& socketPath);

    // DefaultCommand with the environment and the directory of the executable
    // given.
    static Command CommandFor(const QString& socketPath,
                              const QString& appImage, const QString& appDir,
                              const QString& executableDir);

    // Starts the process alone in its own session, with no terminal and no
    // inherited descriptors, in $HOME, so that it outlives the window. Its
    // standard output and error go to `command.StartLog`, else /dev/null. On
    // failure false and the reason in `error`.
    static bool StartDetached(const Command& command, QString& error);

    // `api` is listened to, not owned.
    DaemonLauncher(
        api::Api& api, Command command,
        std::chrono::milliseconds startTimeout = DEFAULT_START_TIMEOUT,
        StartFunction start = &DaemonLauncher::StartDetached,
        QObject* parent = nullptr);

    // Whether a start is in flight; the window shows "Starting roostd…".
    bool Starting() const { return Starting_; }

    // Why the last start failed, empty after a start, a connection or a
    // refusal; the window shows it in the banner.
    const QString& Failure() const { return Failure_; }

signals:
    void StartingChanged(bool starting);
    void FailureChanged();

private:
    void OnNoListener();
    void OnTimeout();
    void SetStarting(bool starting);
    void SetFailure(QString failure);

    Command Command_;
    StartFunction Start_;
    QTimer Timeout_;
    bool Starting_ = false;
    QString Failure_;
};

}  // namespace roost
