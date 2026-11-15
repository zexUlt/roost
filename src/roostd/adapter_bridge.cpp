#include "roostd/adapter_bridge.hpp"

#include <QJsonArray>
#include <QSet>

#include <array>
#include <utility>

#include "roostd/backend.hpp"

namespace roost {

namespace {

struct StateName {
    EAgentState State;
    QLatin1StringView Name;
};

constexpr std::array<StateName, 5> STATE_NAMES{{
    {EAgentState::Working, QLatin1StringView("working")},
    {EAgentState::Blocked, QLatin1StringView("blocked")},
    {EAgentState::Idle, QLatin1StringView("idle")},
    {EAgentState::Done, QLatin1StringView("done")},
    {EAgentState::Failed, QLatin1StringView("failed")},
}};

std::optional<EAgentState> StateFromName(const QString& name) {
    for (const StateName& known : STATE_NAMES) {
        if (name == known.Name) {
            return known.State;
        }
    }
    return std::nullopt;
}

QString NameOf(EAgentState state) {
    for (const StateName& known : STATE_NAMES) {
        if (state == known.State) {
            return known.Name;
        }
    }
    Q_UNREACHABLE_RETURN(QString());
}

// An optional string member: absent leaves `out` as it is.
bool ReadString(const QJsonObject& object, const QString& key, QString& out) {
    const QJsonValue value = object.value(key);
    if (value.isUndefined()) {
        return true;
    }
    if (!value.isString()) {
        return false;
    }
    out = value.toString();
    return true;
}

rpc::Error BadAnswer(const QString& agent, const QString& method,
                     const QString& why) {
    return rpc::Error::FromCode(
        rpc::EErrorCode::InternalError,
        QStringLiteral("the adapter of %1 answered %2 badly: %3")
            .arg(agent, method, why));
}

}  // namespace

QJsonObject ToJson(const AgentSession& session) {
    QJsonObject json{
        {QStringLiteral("agent"), session.Agent},
        {QStringLiteral("id"), session.Id},
        {QStringLiteral("title"), session.Title},
        {QStringLiteral("cwd"), session.WorkingDirectory},
        {QStringLiteral("state"), NameOf(session.State)},
    };
    if (session.StartedAt != 0) {
        json.insert(QStringLiteral("startedAt"), session.StartedAt);
    }
    return json;
}

std::expected<AgentSession, QString> SessionFromJson(const QString& agent,
                                                     const QJsonValue& value) {
    if (!value.isObject()) {
        return std::unexpected(QStringLiteral("a session is not an object"));
    }
    const QJsonObject object = value.toObject();
    AgentSession session;
    session.Agent = agent;
    const QJsonValue id = object.value(QStringLiteral("id"));
    if (!id.isString() || id.toString().isEmpty()) {
        return std::unexpected(QStringLiteral("a session has no id"));
    }
    session.Id = id.toString();
    const QJsonValue state = object.value(QStringLiteral("state"));
    const std::optional<EAgentState> known = StateFromName(state.toString());
    if (!state.isString() || !known) {
        return std::unexpected(
            QStringLiteral("session %1 has no state of working, blocked, "
                           "idle, done or failed")
                .arg(session.Id));
    }
    session.State = *known;
    if (!ReadString(object, QStringLiteral("title"), session.Title) ||
        !ReadString(object, QStringLiteral("cwd"), session.WorkingDirectory)) {
        return std::unexpected(
            QStringLiteral("the title and cwd of session %1 must be strings")
                .arg(session.Id));
    }
    const QJsonValue startedAt = object.value(QStringLiteral("startedAt"));
    if (!startedAt.isUndefined()) {
        const std::optional<qint64> milliseconds = rpc::ToInteger(startedAt);
        if (!milliseconds || *milliseconds <= 0) {
            return std::unexpected(
                QStringLiteral("startedAt of session %1 must be milliseconds "
                               "since the epoch")
                    .arg(session.Id));
        }
        session.StartedAt = *milliseconds;
    }
    return session;
}

std::expected<std::optional<PtySpec>, QString> AttachCommandFromJson(
    const QJsonValue& value) {
    if (value.isNull()) {
        return std::nullopt;
    }
    const QJsonObject object = value.toObject();
    const QJsonValue program = object.value(QStringLiteral("program"));
    if (!value.isObject() || !program.isString() ||
        program.toString().isEmpty()) {
        return std::unexpected(QStringLiteral(
            "an attach command is null or {program, args?, cwd?, env?}"));
    }
    PtySpec spec;
    spec.Program = program.toString();
    const QJsonValue args = object.value(QStringLiteral("args"));
    if (!args.isUndefined() && !args.isArray()) {
        return std::unexpected(QStringLiteral("args must be an array"));
    }
    for (const QJsonValue& arg : args.toArray()) {
        if (!arg.isString()) {
            return std::unexpected(QStringLiteral("args must be strings"));
        }
        spec.Arguments.append(arg.toString());
    }
    if (!ReadString(object, QStringLiteral("cwd"), spec.WorkingDirectory)) {
        return std::unexpected(QStringLiteral("cwd must be a string"));
    }
    const QJsonValue env = object.value(QStringLiteral("env"));
    if (!env.isUndefined() && !env.isObject()) {
        return std::unexpected(QStringLiteral("env must be an object"));
    }
    const QJsonObject variables = env.toObject();
    for (auto it = variables.begin(); it != variables.end(); ++it) {
        if (it.key().isEmpty() || it.key().contains(u'=') ||
            !it.value().isString()) {
            return std::unexpected(
                QStringLiteral("env must map names without '=' to strings"));
        }
        spec.Environment.append(it.key() + u'=' + it.value().toString());
    }
    return spec;
}

AdapterBridge::AdapterBridge(QString agent, QStringList capabilities,
                             Backend& backend)
    : Agent_(std::move(agent)),
      Capabilities_(std::move(capabilities)),
      Backend_(backend) {}

QFuture<AdapterBridge::Answer<std::vector<AgentSession>>>
AdapterBridge::ListSessions() {
    static const QString METHOD = QStringLiteral("agent.listSessions");
    return Backend_
        .Request(METHOD, QJsonObject{{QStringLiteral("agent"), Agent_}})
        .then(QtFuture::Launch::Sync,
              [agent = Agent_](const rpc::Result& result)
                  -> Answer<std::vector<AgentSession>> {
                  if (!result) {
                      return std::unexpected(result.error());
                  }
                  if (!result->isArray()) {
                      return std::unexpected(BadAnswer(
                          agent, METHOD, QStringLiteral("not an array")));
                  }
                  std::vector<AgentSession> sessions;
                  QSet<QString> seen;
                  for (const QJsonValue& entry : result->toArray()) {
                      auto session = SessionFromJson(agent, entry);
                      if (!session) {
                          return std::unexpected(
                              BadAnswer(agent, METHOD, session.error()));
                      }
                      if (seen.contains(session->Id)) {
                          return std::unexpected(BadAnswer(
                              agent, METHOD,
                              QStringLiteral("session %1 is listed twice")
                                  .arg(session->Id)));
                      }
                      seen.insert(session->Id);
                      sessions.push_back(*std::move(session));
                  }
                  return sessions;
              });
}

QFuture<AdapterBridge::Answer<std::optional<PtySpec>>>
AdapterBridge::AttachCommand(const QString& sessionId) {
    static const QString METHOD = QStringLiteral("agent.attachCommand");
    return Backend_
        .Request(METHOD, QJsonObject{{QStringLiteral("agent"), Agent_},
                                     {QStringLiteral("session"), sessionId}})
        .then(QtFuture::Launch::Sync,
              [agent = Agent_](
                  const rpc::Result& result) -> Answer<std::optional<PtySpec>> {
                  if (!result) {
                      return std::unexpected(result.error());
                  }
                  auto spec = AttachCommandFromJson(*result);
                  if (!spec) {
                      return std::unexpected(
                          BadAnswer(agent, METHOD, spec.error()));
                  }
                  return *std::move(spec);
              });
}

}  // namespace roost
