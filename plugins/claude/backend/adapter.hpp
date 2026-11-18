#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QFuture>
#include <QHash>
#include <QObject>
#include <QPromise>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <expected>
#include <functional>
#include <optional>
#include <vector>

#include "claude/backend/agents.hpp"
#include "claude/backend/claude_run.hpp"
#include "claude/backend/json_rpc.hpp"

class QJsonObject;
class QJsonValue;
class QProcess;

namespace roost::claude {

struct AdapterOptions {
    QString Program = QStringLiteral("claude");
    // From the end of one run of `claude agents` to the start of the next.
    std::chrono::milliseconds PollInterval{3'000};
    // A run that takes longer is killed and counts as failed.
    std::chrono::milliseconds PollTimeout{20'000};
    // How long a command may run before its `claude` is killed.
    struct CommandTimeouts {
        std::chrono::milliseconds Stop{60'000};
        std::chrono::milliseconds Remove{60'000};
        std::chrono::milliseconds Respawn{300'000};
        std::chrono::milliseconds RespawnAll{300'000};
        std::chrono::milliseconds NewSession{120'000};
    };
    CommandTimeouts Timeouts;
};

// The Claude adapter (design.md §7): lists Claude Code's background sessions
// by running `<Program> agents --json --all`, one run at a time, and answers
// roostd from the last run:
//
//   agent.listSessions {agent: "claude"} -> [{id, state, title, cwd,
//     startedAt?}]
//   agent.attachCommand {agent: "claude", session} -> {program, args} that
//     runs `<Program> attach <session>`, or null for a session it does not
//     list
//
// commands.run {command, args?} runs one command of the plugin (plugin.json):
//
//   claude.stop {session}        `<Program> stop <session>`
//   claude.remove {session}      `<Program> rm <session>`
//   claude.respawn {session}     `<Program> respawn <session>`
//   claude.respawnAll {}         `<Program> respawn --all`
//   claude.newSession {cwd, prompt, flags?}
//       `<Program> --bg <flags...> -- <prompt>` in the directory cwd
//
// The result of the first four is null. The result of newSession is
// {agent: "claude", id, title}: the id and name from the line
// `backgrounded · <id> · <name>` that claude prints once it started the
// session, title being the id if there is no name. Exit 0 without that line
// is InternalError: the last line of the output as the message, or `claude
// --bg printed no session id`. cwd is an absolute path of an existing
// directory, as the adapter does not run in a directory of its own choice;
// prompt is not empty and, like each flag, has no NUL (argv would cut it);
// cwd is a directory that can be read and entered; flags is an optional array
// of strings, which the caller owns (model, permission mode). Other keys of
// args are ignored. Claude refuses a directory it does not trust with exit 1
// and its reason, which is the message as for any failed run.
//
// The result of the others is null. A session is an id of letters, digits, `_`
// and `-` that does not start with `-`: it is one argument for claude, never an
// option. Anything else, an unknown command included, is InvalidParams and runs
// nothing. A run that exits with a code other than 0 is InternalError: the
// reason line of claude as the message, {exitCode, stdout, stderr} as data.
// A run that cannot start, crashes or takes longer than its timeout is
// InternalError with the reason. Commands run beside the polling and each
// other, and each ends with a run of `claude agents` at once, so that its
// effect is sent without waiting for the poll timer.
//
// Requests wait for the first run. While the last run failed, both fail with
// its reason. After every successful run but the first one, each session
// that is new or changed its state since the previous successful run is sent
// as session.stateChanged {agent, session}. Each session of the previous
// successful run that the new one does not list is sent after them as
// session.removed {agent, session: <id>}, in the order of the previous
// listing. A failed run sends nothing: a removal during failed runs is sent
// after the next successful one. A restarted process has no previous listing
// and sends none of this for what changed while it was down: roostd announces
// the restart and the window lists the sessions again.
class Adapter : public QObject {
    Q_OBJECT

public:
    static constexpr QLatin1StringView AGENT{"claude"};

    // Sends one compact JSON-RPC message to roostd.
    using Send = std::function<void(const QByteArray&)>;

    Adapter(AdapterOptions options, Send send, QObject* parent = nullptr);
    // Requests still waiting for the first run fail.
    ~Adapter() override;

    Adapter(const Adapter&) = delete;
    Adapter& operator=(const Adapter&) = delete;

    // Starts the first run.
    void Start();

    // One message from roostd. The future yields the response body, or an
    // empty array if none is due.
    QFuture<QByteArray> Handle(QByteArrayView message);

private:
    struct Waiting {
        std::function<jsonrpc::Result()> Answer;
        QPromise<jsonrpc::Result> Promise;
    };

    void Poll();
    // A run of `claude agents` now: at once if none is going, else right after
    // the one that is, which may have started before the change.
    void PollSoon();
    QFuture<jsonrpc::Result> RunCommand(const QJsonObject& params);
    QFuture<jsonrpc::Result> StartSession(const QJsonValue& args);
    // One run of claude for a command: failures become errors, a run with exit
    // 0 is answered by onSuccess, and a run of `claude agents` follows.
    QFuture<jsonrpc::Result> Launch(
        RunRequest request, QString verb,
        std::function<jsonrpc::Result(const RunOutput&)> onSuccess);
    void OnPolled(std::expected<Listing, QString> listing);
    // Sends one JSON-RPC notification to roostd.
    void Notify(const QString& method, const QJsonObject& params);
    // The answer now if a run has finished, else once the first one does.
    QFuture<jsonrpc::Result> WhenPolled(
        std::function<jsonrpc::Result()> answer);
    jsonrpc::Result ListSessions() const;
    jsonrpc::Result AttachCommand(const QString& sessionId) const;

    AdapterOptions Options_;
    Send Send_;
    jsonrpc::Dispatcher Dispatcher_;

    QProcess* Process_ = nullptr;
    QTimer PollTimer_;
    QTimer TimeoutTimer_;
    // A run was asked for while another was going.
    bool PollAgain_ = false;
    // The destructor is running: commands that end do not start runs.
    bool Stopping_ = false;

    // The last run: its sessions or why it failed. Empty before the first.
    std::optional<std::expected<std::vector<Session>, QString>> Last_;
    // States from the last run that succeeded, to tell what changed.
    std::optional<QHash<QString, QString>> Known_;
    // The ids of that run in its order, to send removals deterministically.
    QStringList KnownIds_;
    std::vector<Waiting> Waiting_;
    // Warnings already logged: the same one comes with every run.
    QSet<QString> Warned_;
};

}  // namespace roost::claude
