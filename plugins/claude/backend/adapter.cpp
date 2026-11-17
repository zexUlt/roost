#include "claude/backend/adapter.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>

#include <signal.h>

#include <utility>

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

// A run that timed out, or one still going when the adapter stops: its
// process group too, so what claude started does not outlive it. The group is
// killed only while its leader runs: until then its id cannot be reused.
void StopRun(QProcess& process) {
    if (process.state() != QProcess::NotRunning) {
        ::killpg(static_cast<pid_t>(process.processId()), SIGKILL);
        process.kill();
        process.waitForFinished();
    }
}

// ": <the first line of stderr>", or nothing if it is empty.
QString StderrSuffix(const QByteArray& text) {
    const QString line = QString::fromUtf8(text.trimmed()).section(u'\n', 0, 0);
    return line.isEmpty() ? QString() : QStringLiteral(": ") + line;
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
            });
    Q_ASSERT(registered);
    Q_UNUSED(registered);
}

Adapter::~Adapter() {
    if (Process_ != nullptr) {
        Process_->disconnect(this);
        StopRun(*Process_);
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
        StopRun(*process);
        process->deleteLater();
        OnPolled(std::move(result));
        PollTimer_.start(Options_.PollInterval);
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
        for (const Session& session : listing->Sessions) {
            states.insert(session.Id, session.State);
            if (Known_ && Known_->value(session.Id) != session.State) {
                Send_(
                    QJsonDocument(
                        QJsonObject{
                            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                            {QStringLiteral("method"),
                             QStringLiteral("session.stateChanged")},
                            {QStringLiteral("params"),
                             QJsonObject{
                                 {QStringLiteral("agent"), AGENT},
                                 {QStringLiteral("session"), ToJson(session)},
                             }},
                        })
                        .toJson(QJsonDocument::Compact));
            }
        }
        Known_ = std::move(states);
        Last_ = std::move(listing->Sessions);
    }
    std::vector<Waiting> waiting = std::exchange(Waiting_, {});
    for (Waiting& one : waiting) {
        Settle(one.Promise, one.Answer());
    }
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
