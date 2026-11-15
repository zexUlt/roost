#pragma once

#include <QFuture>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <expected>
#include <optional>
#include <vector>

#include "core/pty.hpp"
#include "core/rpc.hpp"

namespace roost {

class Backend;

// The states every agent's sessions share (design.md §7).
enum class EAgentState {
    Working,
    Blocked,  // waits for a human
    Idle,
    Done,
    Failed,
};

// A session of an agent, as roostd and the window see it.
struct AgentSession {
    QString Agent;
    QString Id;
    QString Title;
    QString WorkingDirectory;
    EAgentState State = EAgentState::Idle;
    // Milliseconds since the epoch; 0 if the agent does not tell.
    qint64 StartedAt = 0;

    bool operator==(const AgentSession&) const = default;
};

// {agent, id, title, cwd, state, startedAt?}, state in lowercase.
QJsonObject ToJson(const AgentSession& session);

// A session as an adapter sends it: {id, state, title?, cwd?, startedAt?}.
// Unknown keys are ignored.
std::expected<AgentSession, QString> SessionFromJson(const QString& agent,
                                                     const QJsonValue& value);

// An attach command as an adapter sends it: {program, args?, cwd?, env?},
// env being {NAME: value}; null if the session cannot be attached to.
std::expected<std::optional<PtySpec>, QString> AttachCommandFromJson(
    const QJsonValue& value);

// The AgentAdapter of design.md §7 for one agent, over JSON-RPC to the
// backend of the plugin that serves it. roostd asks the backend
//
//   agent.listSessions  {agent}           [session...]
//   agent.attachCommand {agent, session}  {program, args?, cwd?, env?} | null
//
// An answer that breaks this shape fails with InternalError; the backend's
// own errors come through as they are. The backend must outlive the bridge.
class AdapterBridge {
public:
    template <typename TValue>
    using Answer = std::expected<TValue, rpc::Error>;

    AdapterBridge(QString agent, QStringList capabilities, Backend& backend);

    const QString& Agent() const { return Agent_; }
    const QStringList& Capabilities() const { return Capabilities_; }

    // In the order the adapter gives; no session twice.
    QFuture<Answer<std::vector<AgentSession>>> ListSessions();

    QFuture<Answer<std::optional<PtySpec>>> AttachCommand(
        const QString& sessionId);

private:
    QString Agent_;
    QStringList Capabilities_;
    Backend& Backend_;
};

}  // namespace roost
