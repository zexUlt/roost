#include "notifications/backend/notifier.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>

#include <algorithm>
#include <ranges>

#include "notifications/backend/diagnostics.hpp"

namespace roost::notifications {

namespace {

const QString PLUGIN = QStringLiteral("notifications");
const QString TEST_COMMAND = QStringLiteral("notifications.test");
const QString TEST_MESSAGE = QStringLiteral("Test notification from roost");

jsonrpc::Result InvalidParams(const QString& message) {
    return std::unexpected(
        jsonrpc::Error::FromCode(jsonrpc::EErrorCode::InvalidParams, message));
}

QString MessageFor(EEvent event, const QString& title) {
    switch (event) {
        case EEvent::Blocked:
            return QStringLiteral("%1 needs you").arg(title);
        case EEvent::Done:
            return QStringLiteral("%1 is done").arg(title);
        case EEvent::Failed:
            return QStringLiteral("%1 failed").arg(title);
    }
    return title;
}

// The reason of an error response, for a warning.
QString ReasonOf(const QJsonObject& response) {
    return response.value(QStringLiteral("error"))
        .toObject()
        .value(QStringLiteral("message"))
        .toString();
}

}  // namespace

Notifier::Notifier(NotifierOptions options, Send send, QObject* parent)
    : QObject(parent),
      Send_(std::move(send)),
      Runner_(
          options.Runner,
          [this](const CommandResult& result) { OnResult(result); }, this) {
    const bool registered = Dispatcher_.Register(
        QStringLiteral("commands.run"), [this](const jsonrpc::Call& call) {
            const QJsonObject params = call.Params.toObject();
            if (params.value(QStringLiteral("command")).toString() !=
                TEST_COMMAND) {
                return jsonrpc::Ready(InvalidParams(
                    QStringLiteral("unknown command %1")
                        .arg(params.value(QStringLiteral("command"))
                                 .toString())));
            }
            return jsonrpc::Ready(
                RunTest(params.value(QStringLiteral("args"))));
        });
    Q_ASSERT(registered);
    Q_UNUSED(registered);
}

Notifier::~Notifier() = default;

void Notifier::Start() {
    // Subscribed before the document is read: a change between the two is
    // seen as an event, and the later of the two wins.
    for (const QString& pattern :
         {QStringLiteral("session.*"), QStringLiteral("store.changed")}) {
        SendRequest(QStringLiteral("bus.subscribe"),
                    QJsonObject{{QStringLiteral("pattern"), pattern}},
                    [pattern](const QJsonObject& response) {
                        if (response.contains(QStringLiteral("error"))) {
                            Warn(QStringLiteral("cannot subscribe to %1: %2")
                                     .arg(pattern, ReasonOf(response)));
                        }
                    });
    }
    SendRequest(QStringLiteral("store.get"), QJsonObject{},
                [this](const QJsonObject& response) {
                    const QJsonValue document =
                        response.value(QStringLiteral("result"))
                            .toObject()
                            .value(QStringLiteral("document"));
                    if (document.isObject()) {
                        ApplyDocument(document.toObject());
                    } else {
                        Warn(QStringLiteral("cannot read the document: %1")
                                 .arg(ReasonOf(response)));
                    }
                });
}

QFuture<QByteArray> Notifier::Handle(QByteArrayView message) {
    const QJsonDocument parsed = QJsonDocument::fromJson(message.toByteArray());
    if (parsed.isObject()) {
        const QJsonObject object = parsed.object();
        const bool hasId = object.contains(QStringLiteral("id"));
        const QJsonValue method = object.value(QStringLiteral("method"));
        if (!method.isString() && hasId) {
            OnResponse(object);
            return QtFuture::makeReadyValueFuture(QByteArray());
        }
        if (!hasId && method.toString() == QStringLiteral("bus.event")) {
            OnBusEvent(object.value(QStringLiteral("params")).toObject());
            return QtFuture::makeReadyValueFuture(QByteArray());
        }
    }
    return Dispatcher_.Handle(message);
}

void Notifier::SendRequest(const QString& method, const QJsonValue& params,
                           Answer answer) {
    const qint64 id = NextId_++;
    Pending_.emplace(id, std::move(answer));
    Send_(QJsonDocument(QJsonObject{
                            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                            {QStringLiteral("id"), id},
                            {QStringLiteral("method"), method},
                            {QStringLiteral("params"), params},
                        })
              .toJson(QJsonDocument::Compact));
}

void Notifier::OnResponse(const QJsonObject& response) {
    const auto id = jsonrpc::ToInteger(response.value(QStringLiteral("id")));
    if (!id) {
        return;
    }
    const auto pending = Pending_.find(*id);
    if (pending == Pending_.end()) {
        return;
    }
    const Answer answer = std::move(pending->second);
    Pending_.erase(pending);
    answer(response);
}

void Notifier::OnBusEvent(const QJsonObject& params) {
    const QString name = params.value(QStringLiteral("name")).toString();
    const QJsonValue payload = params.value(QStringLiteral("payload"));
    if (name == QStringLiteral("session.stateChanged")) {
        OnStateChanged(payload.toObject());
    } else if (name == QStringLiteral("session.removed")) {
        OnRemoved(payload.toObject());
    } else if (name == QStringLiteral("session.adapterStarted")) {
        OnAdapterStarted(payload.toObject());
    } else if (name == QStringLiteral("store.changed") &&
               payload.toObject().value(QStringLiteral("plugin")).toString() ==
                   PLUGIN) {
        ApplyDocument(
            payload.toObject().value(QStringLiteral("document")).toObject());
    }
}

void Notifier::OnStateChanged(const QJsonObject& session) {
    Session seen{
        .Agent = session.value(QStringLiteral("agent")).toString(),
        .Id = session.value(QStringLiteral("id")).toString(),
        .Title = session.value(QStringLiteral("title")).toString(),
        .WorkingDirectory = session.value(QStringLiteral("cwd")).toString(),
        .State = session.value(QStringLiteral("state")).toString(),
    };
    if (seen.Id.isEmpty() || seen.State.isEmpty()) {
        return;
    }
    const auto key = std::pair{seen.Agent, seen.Id};
    // The adapters send session.stateChanged only for a change (the first
    // list of an adapter is the baseline and sends nothing), so a session seen
    // for the first time is a change too. The same state again is not: an
    // adapter may repeat it for a new title.
    const auto known = States_.find(key);
    if (known != States_.end() && known->second == seen.State) {
        return;
    }
    States_.insert_or_assign(key, seen.State);
    if (const auto event = EventFromName(seen.State)) {
        Notify(*event, seen);
    }
}

void Notifier::OnAdapterStarted(const QJsonObject& started) {
    // A state kept from before the restart must not swallow the next change:
    // the adapter does not tell what changed while it was down.
    const QJsonArray agents = started.value(QStringLiteral("agents")).toArray();
    for (const QJsonValue& agent : agents) {
        const QString id = agent.toString();
        std::erase_if(States_, [&id](const auto& entry) {
            return entry.first.first == id;
        });
    }
}

void Notifier::OnRemoved(const QJsonObject& removed) {
    States_.erase({removed.value(QStringLiteral("agent")).toString(),
                   removed.value(QStringLiteral("id")).toString()});
}

void Notifier::ApplyDocument(const QJsonObject& document) {
    const QJsonValue settings = document.value(QStringLiteral("settings"));
    if (SettingsRead_ && *SettingsRead_ == settings) {
        return;
    }
    SettingsRead_ = settings;
    Settings_ = ParseSettings(settings);
    ForgetResultsOfRemovedCommands(document.value(QStringLiteral("results")));
}

void Notifier::ForgetResultsOfRemovedCommands(const QJsonValue& results) {
    QJsonObject gone;
    for (const QString& name : results.toObject().keys()) {
        const bool present = std::ranges::any_of(
            Settings_.Commands,
            [&name](const CommandSpec& spec) { return spec.Name == name; });
        if (!present) {
            gone.insert(name, QJsonValue(QJsonValue::Null));
        }
    }
    if (gone.isEmpty()) {
        return;
    }
    SendRequest(QStringLiteral("store.patch"),
                QJsonObject{{QStringLiteral("patch"),
                             QJsonObject{{QStringLiteral("results"), gone}}}},
                [](const QJsonObject& response) {
                    if (response.contains(QStringLiteral("error"))) {
                        Warn(QStringLiteral("cannot drop old results: %1")
                                 .arg(ReasonOf(response)));
                    }
                });
}

void Notifier::Notify(EEvent event, const Session& session) {
    if (!Settings_.Events.contains(event)) {
        return;
    }
    const QString name = NameOf(event);
    const QString message =
        MessageFor(event, session.Title.isEmpty() ? session.Id : session.Title);
    for (const CommandSpec& spec : Settings_.Commands) {
        if (spec.Enabled && (!spec.Events || spec.Events->contains(event))) {
            Runner_.Submit(MakeJob(spec, name, message, session));
        }
    }
}

jsonrpc::Result Notifier::RunTest(const QJsonValue& args) {
    if (!args.isUndefined() && !args.isNull() && !args.isObject()) {
        return InvalidParams(
            QStringLiteral("%1 takes {name?}").arg(TEST_COMMAND));
    }
    const QJsonValue name = args.toObject().value(QStringLiteral("name"));
    if (!name.isUndefined() && !name.isNull() && !name.isString()) {
        return InvalidParams(
            QStringLiteral("%1 takes {name?}: the name of a command")
                .arg(TEST_COMMAND));
    }
    std::vector<const CommandSpec*> chosen;
    for (const CommandSpec& spec : Settings_.Commands) {
        if (name.isString() ? spec.Name == name.toString() : spec.Enabled) {
            chosen.push_back(&spec);
        }
    }
    if (name.isString() && chosen.empty()) {
        return InvalidParams(QStringLiteral("no command %1 in the settings")
                                 .arg(name.toString()));
    }
    QJsonArray ran;
    for (const CommandSpec* spec : chosen) {
        if (Runner_.Submit(MakeJob(*spec, QStringLiteral("test"), TEST_MESSAGE,
                                   Session{}))) {
            ran.append(spec->Name);
        }
    }
    return QJsonObject{{QStringLiteral("ran"), ran}};
}

CommandJob Notifier::MakeJob(const CommandSpec& spec, const QString& event,
                             const QString& message,
                             const Session& session) const {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("ROOST_NOTIFY_VERSION"),
                       QStringLiteral("1"));
    environment.insert(QStringLiteral("ROOST_EVENT"), event);
    environment.insert(QStringLiteral("ROOST_AGENT"), session.Agent);
    environment.insert(QStringLiteral("ROOST_SESSION_ID"), session.Id);
    environment.insert(QStringLiteral("ROOST_SESSION_TITLE"), session.Title);
    environment.insert(QStringLiteral("ROOST_SESSION_CWD"),
                       session.WorkingDirectory);
    environment.insert(QStringLiteral("ROOST_STATE"), session.State);
    environment.insert(QStringLiteral("ROOST_MESSAGE"), message);
    const QJsonObject input{
        {QStringLiteral("version"), 1},
        {QStringLiteral("event"), event},
        {QStringLiteral("agent"), session.Agent},
        {QStringLiteral("sessionId"), session.Id},
        {QStringLiteral("title"), session.Title},
        {QStringLiteral("cwd"), session.WorkingDirectory},
        {QStringLiteral("state"), session.State},
        {QStringLiteral("message"), message},
    };
    return CommandJob{
        .Name = spec.Name,
        .Run = spec.Run,
        .Event = event,
        .Environment = environment,
        .Input = QJsonDocument(input).toJson(QJsonDocument::Compact),
    };
}

void Notifier::OnResult(const CommandResult& result) {
    const QJsonObject entry{
        {QStringLiteral("exitCode"), result.ExitCode
                                         ? QJsonValue(*result.ExitCode)
                                         : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("error"), result.Error.isEmpty()
                                      ? QJsonValue(QJsonValue::Null)
                                      : QJsonValue(result.Error)},
        {QStringLiteral("stderr"), result.Stderr},
        {QStringLiteral("at"), result.At},
        {QStringLiteral("event"), result.Event},
    };
    const QString name = result.Name;
    SendRequest(QStringLiteral("store.patch"),
                QJsonObject{{QStringLiteral("patch"),
                             QJsonObject{{QStringLiteral("results"),
                                          QJsonObject{{name, entry}}}}}},
                [name](const QJsonObject& response) {
                    if (response.contains(QStringLiteral("error"))) {
                        Warn(QStringLiteral("cannot keep the result of %1: %2")
                                 .arg(name, ReasonOf(response)));
                    }
                });
}

}  // namespace roost::notifications
