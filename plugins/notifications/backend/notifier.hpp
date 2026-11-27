#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QFuture>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <functional>
#include <map>
#include <optional>
#include <utility>

#include "common/json_rpc.hpp"
#include "notifications/backend/command_runner.hpp"
#include "notifications/backend/settings.hpp"

namespace roost::notifications {

namespace jsonrpc = plugin::jsonrpc;

struct NotifierOptions {
    RunnerOptions Runner;
};

// The backend of the notifications plugin: runs the commands of the user
// when a session needs a human, is done or failed, so that it works while no
// window runs and comes once however many do.
//
// It asks roostd, as the backend of a plugin (docs/design/05-plugins.md), for
// the events `session.*` and `store.changed` (bus.subscribe) and for its own
// document (store.get); it writes the outcome of each command with
// store.patch.
//
// The document of the plugin has two keys. `settings` is written by the user
// and read here, at the start and at each store.changed of the plugin:
//
//   {"desktop": true,                      the window shows it (below)
//    "events": ["blocked", "failed"],      the events that notify at all
//    "commands": [{"name": "phone",        names are unique
//                  "run": "my-notifier --text \"$ROOST_MESSAGE\"",
//                  "events": ["blocked"],  default: all three
//                  "enabled": true}]}      default: true
//
// A field of the wrong type is skipped with a warning and the rest is used;
// a missing `settings` or field is the default shown above. `results` is
// written here and nowhere else, by store.patch of {"results": {<name>:
// {...}}}, so that `settings` is never overwritten:
//
//   {"exitCode": n, "error": "timeout"|"failed to start"|"crashed",
//    "stderr": "<last 2000 bytes, from a character boundary>", "at": <ms since
//    the epoch>, "event": "<event>"}
//
// "exitCode" is there for a command that exited, "error" for one that did
// not; the other is absent (a null of a patch deletes the key).
//
// An event is a session whose state changes into blocked, done or failed. The
// adapters take the baseline themselves and send session.stateChanged only for
// a change, so a session not seen before is a change, and a session first
// reported as blocked notifies. The same state again notifies nothing (an
// adapter may repeat it for a new title). session.removed forgets the session,
// and session.adapterStarted forgets the sessions of the restarted agents, so
// that a state kept from before the restart does not swallow the next change.
// A command runs for an event that is in `events` and in its own `events`
// unless it is not `enabled`.
//
// When `settings` changes, the `results` of the commands that are no longer in
// it are deleted.
//
// With `desktop` on, an event that is in `events` is also published on the bus
// as `notifications.show` {title, body, agent, session}, for the windows: the
// title is the title of the session (the id for one without), the body is
// "needs you", "is done" or "failed". The commands run whatever `desktop` is.
//
// A command is `bash -c <run>`, never with data of the session in the text. It
// gets the environment of the backend and
//
//   ROOST_NOTIFY_VERSION=1  ROOST_EVENT  ROOST_AGENT  ROOST_SESSION_ID
//   ROOST_SESSION_TITLE  ROOST_SESSION_CWD  ROOST_STATE  ROOST_MESSAGE
//
// and the same on stdin as one JSON object, the names without ROOST_ in camel
// case: {version, event, agent, sessionId, title, cwd, state, message}.
// ROOST_MESSAGE is "<title> needs you", "<title> is done" or "<title>
// failed", the id for a session without a title.
//
// commands.run {command: "notifications.test", args: {name?}} runs the
// command `name`, even a disabled one, or every enabled command, whatever
// their events, with ROOST_EVENT=test and the message "Test notification from
// roost", and answers {ran: [names]} at once: the commands that were started
// or queued behind the running ones (a command dropped for a full queue is not
// in it), not those that have run. With `desktop` on it also publishes
// `notifications.show` {title: "roost", body: "Test notification from roost"},
// with no session. A name that is not in the settings is InvalidParams.
class Notifier : public QObject {
    Q_OBJECT

public:
    // Sends one compact JSON-RPC message to roostd.
    using Send = std::function<void(const QByteArray&)>;

    Notifier(NotifierOptions options, Send send, QObject* parent = nullptr);
    ~Notifier() override;

    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;

    // Subscribes and reads the document.
    void Start();

    // One message from roostd. The future yields the response body, or an
    // empty array if none is due.
    QFuture<QByteArray> Handle(QByteArrayView message);

private:
    // What the commands are told of a session.
    struct Session {
        QString Agent;
        QString Id;
        QString Title;
        QString WorkingDirectory;
        QString State;
    };

    using Answer = std::function<void(const QJsonObject& response)>;

    void SendRequest(const QString& method, const QJsonValue& params,
                     Answer answer);
    void OnResponse(const QJsonObject& response);
    void OnBusEvent(const QJsonObject& params);
    void OnStateChanged(const QJsonObject& session);
    void OnRemoved(const QJsonObject& removed);
    void OnAdapterStarted(const QJsonObject& started);
    void ForgetResultsOfRemovedCommands(const QJsonValue& results);
    void ApplyDocument(const QJsonObject& document);
    void Notify(EEvent event, const Session& session);
    // Publishes notifications.show; no agent and session in the payload when
    // `session` is empty.
    void PublishShow(const QString& title, const QString& body,
                     const QString& agent, const QString& session);
    jsonrpc::Result RunTest(const QJsonValue& args);
    CommandJob MakeJob(const CommandSpec& spec, const QString& event,
                       const QString& message, const Session& session) const;
    void OnResult(const CommandResult& result);

    Send Send_;
    jsonrpc::Dispatcher Dispatcher_;
    qint64 NextId_ = 1;
    std::map<qint64, Answer> Pending_;
    Settings Settings_;
    // The `settings` value last parsed, to warn once for a bad field and not
    // at each of the changes of `results` that this backend causes itself.
    std::optional<QJsonValue> SettingsRead_;
    // The last state of each session, by (agent, id).
    std::map<std::pair<QString, QString>, QString> States_;
    CommandRunner Runner_;
};

}  // namespace roost::notifications
