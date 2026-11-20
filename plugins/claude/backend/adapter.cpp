#include "claude/backend/adapter.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#include "claude/backend/claude_run.hpp"
#include "common/backend_process.hpp"

namespace roost::claude {

namespace {

void Settle(QPromise<jsonrpc::Result>& promise, jsonrpc::Result result) {
    promise.addResult(std::move(result));
    promise.finish();
}

jsonrpc::Result InvalidParams(const QString& message) {
    return std::unexpected(
        jsonrpc::Error::FromCode(jsonrpc::EErrorCode::InvalidParams, message));
}

jsonrpc::Result Failed(const QString& message) {
    return std::unexpected(
        jsonrpc::Error::FromCode(jsonrpc::EErrorCode::InternalError, message));
}

bool IsOurs(const QJsonObject& params) {
    return params.value(QStringLiteral("agent")).toString() == Adapter::AGENT;
}

// ": <the first line of stderr>", or nothing if it is empty.
QString StderrSuffix(const QByteArray& text) {
    const QString line = QString::fromUtf8(text.trimmed()).section(u'\n', 0, 0);
    return line.isEmpty() ? QString() : QStringLiteral(": ") + line;
}

// A command of the plugin and the `claude` run behind it.
struct CommandSpec {
    QLatin1StringView Id;
    // The first argument of claude.
    QLatin1StringView Verb;
    // The session id follows the verb; else `--all` does.
    bool TakesSession;
    std::chrono::milliseconds AdapterOptions::CommandTimeouts::* Timeout;
};

constexpr QLatin1StringView NEW_SESSION{"claude.newSession"};

constexpr std::array<CommandSpec, 4> COMMANDS{{
    {QLatin1StringView("claude.stop"), QLatin1StringView("stop"), true,
     &AdapterOptions::CommandTimeouts::Stop},
    {QLatin1StringView("claude.remove"), QLatin1StringView("rm"), true,
     &AdapterOptions::CommandTimeouts::Remove},
    {QLatin1StringView("claude.respawn"), QLatin1StringView("respawn"), true,
     &AdapterOptions::CommandTimeouts::Respawn},
    {QLatin1StringView("claude.respawnAll"), QLatin1StringView("respawn"),
     false, &AdapterOptions::CommandTimeouts::RespawnAll},
}};

// The longest stdout or stderr that goes into an error.
constexpr qsizetype MAX_ERROR_TEXT = 4096;

QString ErrorText(const QByteArray& text) {
    const QByteArray trimmed = text.trimmed();
    return QString::fromUtf8(
        trimmed.first(qMin<qsizetype>(trimmed.size(), MAX_ERROR_TEXT)));
}

// An id of letters, digits, `_` and `-` that does not start with `-`: claude
// would take such an id for an option, and `respawn --all` for every session.
bool IsSessionId(const QString& text) {
    static const QRegularExpression PATTERN(
        QStringLiteral(R"(\A[0-9A-Za-z_][0-9A-Za-z_-]*\z)"));
    return PATTERN.match(text).hasMatch();
}

// The text of a run without terminal colours, other CSI sequences and the CR
// of CRLF line ends.
QString WithoutEscapes(const QByteArray& text) {
    static const QRegularExpression CSI(
        QStringLiteral(R"(\x1b\[[0-9;?]*[ -/]*[@-~])"));
    return QString::fromUtf8(text).remove(CSI).remove(u'\r');
}

// A directory claude can start in: one that is read and entered, else the
// start fails after the check. A directory that goes away later still fails.
bool IsUsableDirectory(const QString& path) {
    const QFileInfo info(path);
    return info.isDir() && info.isReadable() && info.isExecutable();
}

struct Backgrounded {
    QString Id;
    // Empty if claude printed no name.
    QString Name;
};

// The session that `claude --bg` announced: its first line
// `backgrounded · <id> · <name>`. The lines after it are hints.
std::optional<Backgrounded> FindBackgrounded(const RunOutput& output) {
    static const QRegularExpression LINE(
        QStringLiteral(R"(^[ \t]*backgrounded · ([0-9A-Za-z_][0-9A-Za-z_-]*))"
                       R"((?: · ([^\n]*))?$)"),
        QRegularExpression::MultilineOption);
    const QString text = WithoutEscapes(output.StandardOutput) + u'\n' +
                         WithoutEscapes(output.StandardError);
    const QRegularExpressionMatch match = LINE.match(text);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    return Backgrounded{match.captured(1), match.captured(2).trimmed()};
}

// The last line of the text of a run that has words.
QString LastLine(const QByteArray& standardOutput,
                 const QByteArray& standardError) {
    const QStringList lines =
        (WithoutEscapes(standardOutput) + u'\n' + WithoutEscapes(standardError))
            .split(u'\n');
    for (auto line = lines.crbegin(); line != lines.crend(); ++line) {
        if (!line->trimmed().isEmpty()) {
            return line->trimmed();
        }
    }
    return {};
}

QJsonObject FailureData(const RunOutput& output) {
    return {
        {QStringLiteral("exitCode"), output.ExitCode},
        {QStringLiteral("stdout"), ErrorText(output.StandardOutput)},
        {QStringLiteral("stderr"), ErrorText(output.StandardError)},
    };
}

}  // namespace

Adapter::Adapter(AdapterOptions options, Send send, QObject* parent)
    : QObject(parent), Options_(std::move(options)), Send_(std::move(send)) {
    PollTimer_.setSingleShot(true);
    connect(&PollTimer_, &QTimer::timeout, this, &Adapter::Poll);
    TimeoutTimer_.setSingleShot(true);

    const bool registered =
        Dispatcher_.Register(
            QStringLiteral("agent.listSessions"),
            [this](const jsonrpc::Call& call) {
                if (!IsOurs(call.Params.toObject())) {
                    return jsonrpc::Ready(InvalidParams(
                        QStringLiteral("agent.listSessions takes {agent: "
                                       "\"claude\"}")));
                }
                return WhenPolled([this] { return ListSessions(); });
            }) &&
        Dispatcher_.Register(
            QStringLiteral("agent.attachCommand"),
            [this](const jsonrpc::Call& call) {
                const QJsonObject params = call.Params.toObject();
                const QString session =
                    params.value(QStringLiteral("session")).toString();
                if (!IsOurs(params) || session.isEmpty()) {
                    return jsonrpc::Ready(InvalidParams(
                        QStringLiteral("agent.attachCommand takes {agent: "
                                       "\"claude\", session}")));
                }
                return WhenPolled(
                    [this, session] { return AttachCommand(session); });
            }) &&
        Dispatcher_.Register(QStringLiteral("commands.run"),
                             [this](const jsonrpc::Call& call) {
                                 return RunCommand(call.Params.toObject());
                             });
    Q_ASSERT(registered);
    Q_UNUSED(registered);
}

Adapter::~Adapter() {
    Stopping_ = true;
    // Kills the commands still running and fails their requests.
    for (ClaudeRun* run :
         findChildren<ClaudeRun*>(Qt::FindDirectChildrenOnly)) {
        delete run;
    }
    if (Process_ != nullptr) {
        Process_->disconnect(this);
        plugin::StopRun(*Process_);
    }
    for (Waiting& waiting : Waiting_) {
        Settle(waiting.Promise,
               Failed(QStringLiteral("the adapter stopped before claude "
                                     "agents answered")));
    }
}

void Adapter::Start() { Poll(); }

QFuture<QByteArray> Adapter::Handle(QByteArrayView message) {
    return Dispatcher_.Handle(message);
}

void Adapter::Poll() {
    auto* process = new QProcess(this);
    Process_ = process;
    process->setProgram(Options_.Program);
    process->setArguments({QStringLiteral("agents"), QStringLiteral("--json"),
                           QStringLiteral("--all")});
    process->setStandardInputFile(QProcess::nullDevice());
    // A group of its own to kill on a timeout, and default signal handlers,
    // as roostd gives its backends.
    process->setUnixProcessParameters(
        QProcess::UnixProcessFlag::CreateNewSession |
        QProcess::UnixProcessFlag::ResetSignalHandlers);

    // Exactly one of these ends a run; the process is gone after it.
    const auto finish = [this,
                         process](std::expected<Listing, QString> result) {
        if (Process_ != process) {
            return;
        }
        Process_ = nullptr;
        TimeoutTimer_.stop();
        TimeoutTimer_.disconnect();
        process->disconnect(this);
        plugin::StopRun(*process);
        process->deleteLater();
        OnPolled(std::move(result));
        PollTimer_.start(std::exchange(PollAgain_, false)
                             ? std::chrono::milliseconds::zero()
                             : Options_.PollInterval);
    };
    connect(process, &QProcess::errorOccurred, this,
            [process, finish](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    finish(std::unexpected(
                        QStringLiteral("cannot run claude agents: %1")
                            .arg(process->errorString())));
                }
            });
    connect(
        process, &QProcess::finished, this,
        [process, finish](int code, QProcess::ExitStatus status) {
            if (status != QProcess::NormalExit) {
                finish(
                    std::unexpected(QStringLiteral("claude agents crashed")));
            } else if (code != 0) {
                finish(std::unexpected(
                    QStringLiteral("claude agents exited with code %1%2")
                        .arg(code)
                        .arg(StderrSuffix(process->readAllStandardError()))));
            } else {
                finish(ParseAgents(process->readAllStandardOutput()));
            }
        });
    connect(&TimeoutTimer_, &QTimer::timeout, this, [this, finish] {
        finish(std::unexpected(
            QStringLiteral("claude agents did not finish in %1 ms")
                .arg(Options_.PollTimeout.count())));
    });
    TimeoutTimer_.start(Options_.PollTimeout);
    process->start();
}

void Adapter::PollSoon() {
    if (Process_ != nullptr) {
        PollAgain_ = true;
        return;
    }
    PollTimer_.stop();
    Poll();
}

QFuture<jsonrpc::Result> Adapter::RunCommand(const QJsonObject& params) {
    const QString id = params.value(QStringLiteral("command")).toString();
    if (id == NEW_SESSION) {
        return StartSession(params.value(QStringLiteral("args")));
    }
    const auto spec = std::ranges::find(
        COMMANDS, id, [](const CommandSpec& c) { return QString(c.Id); });
    if (spec == COMMANDS.end()) {
        return jsonrpc::Ready(
            InvalidParams(QStringLiteral("unknown command %1").arg(id)));
    }
    // Absent, null or an object: respawnAll takes no arguments but does not
    // mind {}.
    const QJsonValue args = params.value(QStringLiteral("args"));
    QStringList argv{QString(spec->Verb)};
    if (spec->TakesSession) {
        const QString session =
            args.toObject().value(QStringLiteral("session")).toString();
        if (!args.isObject() || !IsSessionId(session)) {
            return jsonrpc::Ready(InvalidParams(
                QStringLiteral("%1 takes {session}: a session id").arg(id)));
        }
        argv.append(session);
    } else {
        if (!args.isUndefined() && !args.isNull() && !args.isObject()) {
            return jsonrpc::Ready(
                InvalidParams(QStringLiteral("%1 takes no arguments").arg(id)));
        }
        argv.append(QStringLiteral("--all"));
    }

    return Launch({.Program = Options_.Program,
                   .Args = argv,
                   .Timeout = Options_.Timeouts.*(spec->Timeout)},
                  QString(spec->Verb), [](const RunOutput&) -> jsonrpc::Result {
                      return QJsonValue();
                  });
}

QFuture<jsonrpc::Result> Adapter::StartSession(const QJsonValue& args) {
    const auto invalid = [] {
        return jsonrpc::Ready(InvalidParams(
            QStringLiteral("claude.newSession takes {cwd, prompt, flags?}: an "
                           "absolute path of a directory, a prompt, and "
                           "strings for claude")));
    };
    if (!args.isObject()) {
        return invalid();
    }
    const QJsonObject object = args.toObject();
    const QJsonValue cwd = object.value(QStringLiteral("cwd"));
    const QJsonValue prompt = object.value(QStringLiteral("prompt"));
    const QJsonValue flags = object.value(QStringLiteral("flags"));
    if (!cwd.isString() || !QDir::isAbsolutePath(cwd.toString()) ||
        !IsUsableDirectory(cwd.toString()) || !prompt.isString() ||
        prompt.toString().trimmed().isEmpty() ||
        prompt.toString().contains(QChar(u'\0')) ||
        (!flags.isUndefined() && !flags.isNull() && !flags.isArray())) {
        return invalid();
    }
    QStringList argv{QStringLiteral("--bg")};
    for (const QJsonValue& flag : flags.toArray()) {
        if (!flag.isString() || flag.toString().contains(QChar(u'\0'))) {
            return invalid();
        }
        argv.append(flag.toString());
    }
    // The prompt is one argument, and no option even if it starts with `-`.
    argv.append(QStringLiteral("--"));
    argv.append(prompt.toString());

    return Launch(
        {.Program = Options_.Program,
         .Args = argv,
         .WorkingDirectory = cwd.toString(),
         .Timeout = Options_.Timeouts.NewSession},
        QStringLiteral("--bg"), [](const RunOutput& output) -> jsonrpc::Result {
            const std::optional<Backgrounded> started =
                FindBackgrounded(output);
            if (!started) {
                const QString last =
                    LastLine(output.StandardOutput, output.StandardError);
                return std::unexpected(jsonrpc::Error::FromCode(
                    jsonrpc::EErrorCode::InternalError,
                    last.isEmpty()
                        ? QStringLiteral("claude --bg printed no session id")
                        : last,
                    FailureData(output)));
            }
            return QJsonObject{
                {QStringLiteral("agent"), AGENT},
                {QStringLiteral("id"), started->Id},
                {QStringLiteral("title"),
                 started->Name.isEmpty() ? started->Id : started->Name},
            };
        });
}

QFuture<jsonrpc::Result> Adapter::Launch(
    RunRequest request, QString verb,
    std::function<jsonrpc::Result(const RunOutput&)> onSuccess) {
    auto* run = new ClaudeRun(std::move(request), this);
    return run->Start().then(
        QtFuture::Launch::Sync,
        [this, verb = std::move(verb), onSuccess = std::move(onSuccess)](
            RunResult result) -> jsonrpc::Result {
            // Success or not, the list may have changed.
            if (!Stopping_) {
                PollSoon();
            }
            if (!result) {
                return Failed(result.error());
            }
            if (result->ExitCode != 0) {
                return std::unexpected(jsonrpc::Error::FromCode(
                    jsonrpc::EErrorCode::InternalError,
                    FailureReason(*result, verb), FailureData(*result)));
            }
            return onSuccess(*result);
        });
}

void Adapter::OnPolled(std::expected<Listing, QString> listing) {
    if (!listing) {
        // A failure is logged when it changes, not on every run.
        if (!Last_ || *Last_ || Last_->error() != listing.error()) {
            qWarning("claude-adapter: %s", qUtf8Printable(listing.error()));
        }
        Last_ = std::unexpected(listing.error());
    } else {
        for (const QString& warning : listing->Warnings) {
            if (!Warned_.contains(warning)) {
                Warned_.insert(warning);
                qWarning("claude-adapter: %s", qUtf8Printable(warning));
            }
        }
        QHash<QString, QString> states;
        QStringList ids;
        for (const Session& session : listing->Sessions) {
            states.insert(session.Id, session.State);
            ids.append(session.Id);
            if (Known_ && Known_->value(session.Id) != session.State) {
                Notify(QStringLiteral("session.stateChanged"),
                       QJsonObject{
                           {QStringLiteral("agent"), AGENT},
                           {QStringLiteral("session"), ToJson(session)},
                       });
            }
        }
        if (Known_) {
            // After the changes of this run, in the order of the last listing.
            for (const QString& id : std::as_const(KnownIds_)) {
                if (!states.contains(id)) {
                    Notify(QStringLiteral("session.removed"),
                           QJsonObject{
                               {QStringLiteral("agent"), AGENT},
                               {QStringLiteral("session"), id},
                           });
                }
            }
        }
        KnownIds_ = std::move(ids);
        Known_ = std::move(states);
        Last_ = std::move(listing->Sessions);
    }
    std::vector<Waiting> waiting = std::exchange(Waiting_, {});
    for (Waiting& one : waiting) {
        Settle(one.Promise, one.Answer());
    }
}

void Adapter::Notify(const QString& method, const QJsonObject& params) {
    Send_(QJsonDocument(QJsonObject{
                            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                            {QStringLiteral("method"), method},
                            {QStringLiteral("params"), params},
                        })
              .toJson(QJsonDocument::Compact));
}

QFuture<jsonrpc::Result> Adapter::WhenPolled(
    std::function<jsonrpc::Result()> answer) {
    if (Last_) {
        return jsonrpc::Ready(answer());
    }
    QPromise<jsonrpc::Result> promise;
    QFuture<jsonrpc::Result> future = promise.future();
    promise.start();
    Waiting_.push_back({std::move(answer), std::move(promise)});
    return future;
}

jsonrpc::Result Adapter::ListSessions() const {
    if (!*Last_) {
        return Failed(Last_->error());
    }
    QJsonArray sessions;
    for (const Session& session : **Last_) {
        sessions.append(ToJson(session));
    }
    return sessions;
}

jsonrpc::Result Adapter::AttachCommand(const QString& sessionId) const {
    if (!*Last_) {
        return Failed(Last_->error());
    }
    for (const Session& session : **Last_) {
        if (session.Id == sessionId) {
            return QJsonObject{
                {QStringLiteral("program"), Options_.Program},
                {QStringLiteral("args"),
                 QJsonArray{QStringLiteral("attach"), sessionId}},
            };
        }
    }
    return QJsonValue();
}

}  // namespace roost::claude
