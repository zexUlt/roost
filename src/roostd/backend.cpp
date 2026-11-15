#include "roostd/backend.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QSignalBlocker>

#include <algorithm>
#include <utility>

#include "core/protocol.hpp"

namespace roost {

namespace {

rpc::Result Unavailable(const QString& reason) {
    return std::unexpected(
        protocol::MakeError(protocol::EErrorCode::BackendUnavailable, reason));
}

void Settle(QPromise<rpc::Result>& promise, rpc::Result result) {
    promise.addResult(std::move(result));
    promise.finish();
}

QByteArray Line(const QJsonObject& message) {
    return QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
}

}  // namespace

Backend::Backend(QString name, QStringList command, QString workingDirectory,
                 BackendOptions options, QObject* parent)
    : QObject(parent),
      Name_(std::move(name)),
      Command_(std::move(command)),
      WorkingDirectory_(std::move(workingDirectory)),
      Options_(options),
      Process_(new QProcess(this)) {
    Process_->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    // In a session of its own, Ctrl-C in roostd's terminal does not reach it.
    // Descriptors are not closed here: that walks the whole descriptor limit
    // on every start. roostd opens its own with FD_CLOEXEC instead.
    Process_->setUnixProcessParameters(
        QProcess::UnixProcessFlag::CreateNewSession |
        QProcess::UnixProcessFlag::ResetSignalHandlers);
    RestartTimer_.setSingleShot(true);
    connect(&RestartTimer_, &QTimer::timeout, this, &Backend::Launch);
    connect(Process_, &QProcess::started, this, &Backend::Started);
    connect(Process_, &QProcess::readyReadStandardOutput, this, &Backend::Read);
    connect(
        Process_, &QProcess::finished, this,
        [this](int exitCode, QProcess::ExitStatus status) {
            // QProcess has read what the process wrote before it exited.
            if (Stopping_) {
                return;
            }
            QString reason = BreakReason_;
            if (reason.isEmpty()) {
                reason =
                    status == QProcess::CrashExit
                        ? QStringLiteral("was killed by a signal")
                        : QStringLiteral("exited with code %1").arg(exitCode);
            }
            OnStopped(reason);
        });
    connect(Process_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                // A crash ends in finished(), handled there. A write to a
                // backend that closed its stdin but lives on fails quietly:
                // the request waits, as for a backend that never answers.
                if (error == QProcess::FailedToStart && !Stopping_) {
                    OnStopped(QStringLiteral("failed to start: %1")
                                  .arg(Process_->errorString()));
                }
            });
}

Backend::~Backend() {
    const QSignalBlocker blocker(this);
    Stopping_ = true;
    RestartTimer_.stop();
    FailPending(QStringLiteral("roostd stopped the backend of %1").arg(Name_));
    if (Process_->state() == QProcess::NotRunning) {
        return;
    }
    const auto timeout = static_cast<int>(Options_.StopTimeout.count());
    Process_->closeWriteChannel();
    if (!Process_->waitForFinished(timeout)) {
        Process_->kill();
        Process_->waitForFinished(timeout);
    }
}

void Backend::Start() {
    if (Process_->state() != QProcess::NotRunning || RestartTimer_.isActive()) {
        return;
    }
    CrashesInARow_ = 0;
    Launch();
}

bool Backend::IsRunning() const {
    return Process_->state() != QProcess::NotRunning;
}

qint64 Backend::ProcessId() const { return Process_->processId(); }

QFuture<rpc::Result> Backend::Request(const QString& method,
                                      const QJsonValue& params) {
    if (!IsRunning()) {
        return rpc::Ready(Unavailable(
            QStringLiteral("the backend of %1 is not running").arg(Name_)));
    }
    const qint64 id = NextId_++;
    QJsonObject request{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("method"), method},
    };
    if (!params.isUndefined()) {
        request.insert(QStringLiteral("params"), params);
    }
    QPromise<rpc::Result> promise;
    promise.start();
    QFuture<rpc::Result> future = promise.future();
    Pending_.emplace(id, std::move(promise));
    Process_->write(Line(request));
    return future;
}

void Backend::Launch() {
    Buffer_.clear();
    BreakReason_.clear();
    Uptime_.start();
    Process_->setProgram(Command_.front());
    Process_->setArguments(Command_.mid(1));
    Process_->setWorkingDirectory(WorkingDirectory_);
    Process_->start();
}

void Backend::Read() {
    // Whatever it says while it stops, nobody waits for.
    if (Stopping_) {
        return;
    }
    Buffer_.append(Process_->readAllStandardOutput());
    qsizetype start = 0;
    while (BreakReason_.isEmpty()) {
        const qsizetype end = Buffer_.indexOf('\n', start);
        if (end < 0) {
            break;
        }
        const QByteArray line = Buffer_.sliced(start, end - start);
        start = end + 1;
        if (!line.trimmed().isEmpty()) {
            Dispatch(line);
        }
    }
    if (!BreakReason_.isEmpty()) {
        Buffer_.clear();
        return;
    }
    Buffer_.remove(0, start);
    if (Buffer_.size() > MAX_LINE_LENGTH) {
        Break(QStringLiteral("sent a line longer than %1 bytes")
                  .arg(MAX_LINE_LENGTH));
    }
}

void Backend::Dispatch(const QByteArray& line) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (!document.isObject()) {
        Break(QStringLiteral("sent a line that is not a JSON-RPC message"));
        return;
    }
    const QJsonObject message = document.object();
    const QJsonValue method = message.value(QStringLiteral("method"));
    if (method.isString()) {
        // A request gets the only answer there is for now.
        if (message.contains(QStringLiteral("id"))) {
            Answer(message.value(QStringLiteral("id")));
        } else {
            emit Notified(method.toString(),
                          message.value(QStringLiteral("params")));
        }
        return;
    }
    const std::optional<qint64> id =
        rpc::ToInteger(message.value(QStringLiteral("id")));
    const auto pending = id ? Pending_.find(*id) : Pending_.end();
    if (pending == Pending_.end()) {
        Break(QStringLiteral("answered a request nobody made"));
        return;
    }
    const QJsonValue failure = message.value(QStringLiteral("error"));
    if (!failure.isUndefined() && !failure.isObject()) {
        // The request fails with the rest, once the backend is down.
        Break(QStringLiteral("sent an error that is not an object"));
        return;
    }
    if (failure.isUndefined() && !message.contains(QStringLiteral("result"))) {
        Break(QStringLiteral("answered with neither a result nor an error"));
        return;
    }
    QPromise<rpc::Result> promise = std::move(pending->second);
    Pending_.erase(pending);

    if (failure.isObject()) {
        const QJsonObject object = failure.toObject();
        Settle(
            promise,
            std::unexpected(rpc::Error{
                .Code = object.value(QStringLiteral("code")).toInt(),
                .Message = object.value(QStringLiteral("message")).toString(),
                .Data = object.value(QStringLiteral("data")),
            }));
        return;
    }
    const QJsonValue result = message.value(QStringLiteral("result"));
    Settle(promise, result.isUndefined() ? QJsonValue() : result);
}

void Backend::Answer(const QJsonValue& id) {
    const rpc::Error error = rpc::Error::FromCode(
        rpc::EErrorCode::MethodNotFound,
        QStringLiteral("Method not found: roostd serves backends nothing yet"));
    Process_->write(Line(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("error"),
         QJsonObject{
             {QStringLiteral("code"), error.Code},
             {QStringLiteral("message"), error.Message},
         }},
    }));
}

void Backend::Break(const QString& reason) {
    if (!BreakReason_.isEmpty()) {
        return;
    }
    BreakReason_ = reason;
    qWarning("roostd: the backend of %s %s; killing it", qUtf8Printable(Name_),
             qUtf8Printable(reason));
    Process_->kill();
}

void Backend::OnStopped(const QString& reason) {
    if (Uptime_.durationElapsed() >= Options_.StableRun) {
        CrashesInARow_ = 0;
    }
    ++CrashesInARow_;
    // The restart is settled before anyone hears of the crash: a slot or a
    // continuation that calls Start then waits for it.
    if (CrashesInARow_ > Options_.MaxRestarts) {
        emit GaveUp(reason);
    } else {
        std::chrono::milliseconds delay = Options_.FirstRestartDelay;
        for (int i = 1; i < CrashesInARow_ && delay < Options_.MaxRestartDelay;
             ++i) {
            delay *= 2;
        }
        delay = std::min(delay, Options_.MaxRestartDelay);
        RestartTimer_.start(delay);
        emit Crashed(reason, delay.count());
    }
    FailPending(QStringLiteral("the backend of %1 %2").arg(Name_, reason));
}

void Backend::FailPending(const QString& reason) {
    // Moved out first: a continuation may call Request.
    auto pending = std::exchange(Pending_, {});
    for (auto& [id, promise] : pending) {
        Settle(promise, Unavailable(reason));
    }
}

}  // namespace roost
