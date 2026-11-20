#include "codex/backend/adapter.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QStandardPaths>

#include <algorithm>
#include <limits>
#include <utility>

#include "common/backend_process.hpp"

namespace roost::codex {

namespace {

constexpr QLatin1StringView AGENT{"codex"};
constexpr int PAGE_SIZE = 100;
constexpr qsizetype MAX_PAGES = 1'000;
constexpr auto MAX_POLL_DURATION = std::chrono::minutes(2);
constexpr qsizetype MAX_TITLE_LENGTH = 200;
constexpr qsizetype MAX_LINE_LENGTH = 16LL * 1024 * 1024;

jsonrpc::Result InvalidParams(const QString& message) {
    return std::unexpected(
        jsonrpc::Error::FromCode(jsonrpc::EErrorCode::InvalidParams, message));
}

jsonrpc::Result Failed(const QString& message) {
    return std::unexpected(
        jsonrpc::Error::FromCode(jsonrpc::EErrorCode::InternalError, message));
}

bool IsSessionId(const QString& id) {
    static const QRegularExpression PATTERN(
        QStringLiteral(R"(\A[0-9A-Za-z_][0-9A-Za-z_-]*\z)"));
    return PATTERN.match(id).hasMatch();
}

QByteArray Line(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

QJsonObject Session(const QJsonObject& thread, const QString& id) {
    QString title = thread.value("name").toString().simplified();
    if (title.isEmpty()) {
        title = thread.value("preview").toString().simplified();
    }
    if (title.isEmpty()) {
        title = id;
    }
    if (title.size() > MAX_TITLE_LENGTH) {
        // Truncate between characters, including surrogate pairs.
        title.truncate(
            MAX_TITLE_LENGTH -
            (title.at(MAX_TITLE_LENGTH - 1).isHighSurrogate() ? 1 : 0));
    }
    QJsonObject row{{"id", id}, {"title", title}, {"state", "idle"}};
    const QString cwd = thread.value("cwd").toString();
    if (!cwd.isEmpty()) {
        row.insert("cwd", cwd);
    }
    const qint64 seconds = thread.value("createdAt").toInteger();
    if (seconds > 0 && seconds <= std::numeric_limits<qint64>::max() / 1000) {
        row.insert("startedAt", seconds * 1000);
    }
    return row;
}

}  // namespace

Adapter::Adapter(QString program, std::chrono::milliseconds pollInterval,
                 std::chrono::milliseconds timeout, Send send)
    : Program_(std::move(program)),
      PollInterval_(pollInterval),
      Timeout_(timeout),
      Send_(std::move(send)) {
    const bool registered =
        Dispatcher_.Register(
            "agent.listSessions",
            [this](const jsonrpc::Call& call) {
                if (call.Params.toObject().value("agent").toString() != AGENT) {
                    return jsonrpc::Ready(
                        InvalidParams("expected agent codex"));
                }
                return AfterFirstPoll();
            }) &&
        Dispatcher_.Register(
            "agent.attachCommand", [this](const jsonrpc::Call& call) {
                const QJsonObject params = call.Params.toObject();
                const QString id = params.value("session").toString();
                if (params.value("agent").toString() != AGENT ||
                    !IsSessionId(id)) {
                    return jsonrpc::Ready(
                        InvalidParams("expected agent codex and a session id"));
                }
                return AfterFirstPoll(id);
            });
    Q_ASSERT(registered);
    Q_UNUSED(registered);

    Process_.setUnixProcessParameters(
        QProcess::UnixProcessFlag::CreateNewSession |
        QProcess::UnixProcessFlag::ResetSignalHandlers);
    PollTimer_.setSingleShot(true);
    TimeoutTimer_.setSingleShot(true);
    TimeoutTimer_.setTimerType(Qt::PreciseTimer);
    connect(&PollTimer_, &QTimer::timeout, this, &Adapter::Poll);
    connect(&TimeoutTimer_, &QTimer::timeout, this, [this] {
        Fail(PollDeadline_.hasExpired()
                 ? "codex app-server poll exceeded its deadline"
                 : "codex app-server timed out while listing threads");
    });
    connect(&Process_, &QProcess::started, this, [this] {
        Request("initialize",
                {{"clientInfo",
                  QJsonObject{{"name", "roost"}, {"version", "0.1.0"}}}});
    });
    connect(&Process_, &QProcess::readyReadStandardOutput, this,
            &Adapter::Read);
    connect(&Process_, &QProcess::readyReadStandardError, this,
            [this] { Process_.readAllStandardError(); });
    connect(&Process_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart ||
                    error == QProcess::WriteError ||
                    error == QProcess::ReadError) {
                    Fail("codex app-server: " + Process_.errorString());
                }
            });
    connect(
        &Process_, &QProcess::finished, this,
        [this](int code, QProcess::ExitStatus status) {
            Fail(status == QProcess::CrashExit
                     ? QStringLiteral("codex app-server crashed")
                     : QStringLiteral("codex app-server exited with code %1")
                           .arg(code));
        });
}

Adapter::~Adapter() {
    StopProcess();
    for (Waiting& waiting : Waiting_) {
        waiting.Promise.addResult(Failed("the Codex adapter stopped"));
        waiting.Promise.finish();
    }
}

void Adapter::Start() { Poll(); }

QFuture<QByteArray> Adapter::Handle(QByteArrayView message) const {
    return Dispatcher_.Handle(message);
}

void Adapter::Poll() {
    PollDeadline_.setRemainingTime(MAX_POLL_DURATION, Qt::PreciseTimer);
    if (Process_.state() == QProcess::Running) {
        RequestPage();
        return;
    }

    Executable_ = QStandardPaths::findExecutable(Program_);
    if (Executable_.isEmpty()) {
        // The CLI is optional. An installation can appear on PATH later.
        Publish();
        return;
    }
    StartReplyTimer();
    Process_.start(Executable_, {"app-server"});
}

void Adapter::StartReplyTimer() {
    const std::chrono::milliseconds remaining(PollDeadline_.remainingTime());
    TimeoutTimer_.start(std::min(Timeout_, remaining));
}

void Adapter::Read() {
    Buffer_.append(Process_.readAllStandardOutput());
    qsizetype start = 0;
    for (qsizetype end = Buffer_.indexOf('\n', start); end >= 0;
         end = Buffer_.indexOf('\n', start)) {
        if (end - start > MAX_LINE_LENGTH) {
            break;
        }
        const QByteArray line = Buffer_.sliced(start, end - start).trimmed();
        start = end + 1;
        if (line.isEmpty()) {
            continue;
        }
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject()) {
            Fail("codex app-server sent invalid JSON");
            return;
        }
        HandleReply(document.object());
        if (Process_.state() == QProcess::NotRunning) {
            return;
        }
    }
    Buffer_.remove(0, start);
    if (Buffer_.size() > MAX_LINE_LENGTH) {
        Fail("codex app-server sent an oversized message");
    }
}

void Adapter::HandleReply(const QJsonObject& reply) {
    // App Server also sends notifications. This client only lists threads.
    if (!PendingId_ || reply.value("id").toInteger(-1) != *PendingId_ ||
        reply.contains("method")) {
        return;
    }
    PendingId_.reset();
    TimeoutTimer_.stop();
    if (PollDeadline_.hasExpired()) {
        Fail("codex app-server poll exceeded its deadline");
        return;
    }
    if (!reply.value("result").isObject()) {
        const QString message = reply.value("error")
                                    .toObject()
                                    .value("message")
                                    .toString()
                                    .simplified()
                                    .left(512);
        Fail(message.isEmpty()
                 ? QStringLiteral("codex app-server sent an invalid reply")
                 : QStringLiteral("codex app-server: ") + message);
        return;
    }
    if (!Initialized_) {
        Initialized_ = true;
        Process_.write(Line({{"method", "initialized"}}));
        RequestPage();
        return;
    }
    HandlePage(reply.value("result").toObject());
}

void Adapter::Request(const QString& method, const QJsonObject& params) {
    PendingId_ = ++NextRequestId_;
    StartReplyTimer();
    Process_.write(
        Line({{"id", *PendingId_}, {"method", method}, {"params", params}}));
}

void Adapter::RequestPage(const QString& cursor) {
    QJsonObject params{
        {"limit", PAGE_SIZE},
        {"modelProviders", QJsonArray{}},
        {"sourceKinds", QJsonArray{"cli", "vscode", "appServer"}}};
    if (!cursor.isEmpty()) {
        params.insert("cursor", cursor);
    }
    Request("thread/list", params);
}

void Adapter::HandlePage(const QJsonObject& page) {
    const QJsonValue rows = page.value("data");
    const QJsonValue cursor = page.value("nextCursor");
    const bool lastPage = cursor.isNull() || cursor.isUndefined();
    if (!rows.isArray() ||
        (!lastPage && (!cursor.isString() || cursor.toString().isEmpty()))) {
        Fail("codex app-server sent an invalid thread/list page");
        return;
    }
    for (const QJsonValue& value : rows.toArray()) {
        const QJsonObject thread = value.toObject();
        const QString id = thread.value("id").toString();
        if (!IsSessionId(id)) {
            qWarning("codex app-server: skipping a thread with an invalid id");
            continue;
        }
        if (!thread.value("ephemeral").toBool() &&
            !PendingSessions_.contains(id)) {
            const QJsonObject row = Session(thread, id);
            PendingSessions_.insert(id, row);
            PendingRows_.append(row);
        }
    }
    if (lastPage) {
        Publish();
        return;
    }
    const QString next = cursor.toString();
    if (Cursors_.contains(next)) {
        Fail("codex app-server repeated a thread/list cursor");
        return;
    }
    if (Cursors_.size() >= MAX_PAGES - 1) {
        Fail(QStringLiteral("codex app-server thread/list exceeded %1 pages")
                 .arg(MAX_PAGES));
        return;
    }
    Cursors_.insert(next);
    RequestPage(next);
}

void Adapter::Publish() {
    // A failed or incomplete listing never removes sessions.
    if (Listed_) {
        for (const QJsonValue& value : PendingRows_) {
            const QJsonObject row = value.toObject();
            const QString id = row.value("id").toString();
            if (Sessions_.value(id) != row) {
                Notify("session.stateChanged", row);
            }
        }
        for (const QJsonValue& value : Rows_) {
            const QString id = value.toObject().value("id").toString();
            if (!PendingSessions_.contains(id)) {
                Notify("session.removed", id);
            }
        }
    }
    Error_.clear();
    Sessions_ = std::move(PendingSessions_);
    Rows_ = std::move(PendingRows_);
    FinishPoll();
}

void Adapter::Fail(const QString& reason) {
    StopProcess();
    Error_ = reason;
    FinishPoll();
}

void Adapter::StopProcess() {
    const QSignalBlocker block(&Process_);
    plugin::StopRun(Process_);
    Buffer_.clear();
    Process_.readAllStandardOutput();
    Process_.readAllStandardError();
    Initialized_ = false;
    PendingId_.reset();
}

void Adapter::FinishPoll() {
    TimeoutTimer_.stop();
    PendingSessions_.clear();
    PendingRows_ = {};
    Cursors_.clear();
    Listed_ = true;
    auto waiting = std::exchange(Waiting_, {});
    for (Waiting& request : waiting) {
        request.Promise.addResult(Answer(request.SessionId));
        request.Promise.finish();
    }
    PollTimer_.start(PollInterval_);
}

void Adapter::Notify(const QString& method, const QJsonValue& session) const {
    Send_(QJsonDocument(
              QJsonObject{{"jsonrpc", "2.0"},
                          {"method", method},
                          {"params", QJsonObject{{"agent", AGENT},
                                                 {"session", session}}}})
              .toJson(QJsonDocument::Compact));
}

QFuture<jsonrpc::Result> Adapter::AfterFirstPoll(const QString& sessionId) {
    if (Listed_) {
        return jsonrpc::Ready(Answer(sessionId));
    }
    Waiting_.push_back(Waiting{sessionId, {}});
    QPromise<jsonrpc::Result>& promise = Waiting_.back().Promise;
    promise.start();
    return promise.future();
}

jsonrpc::Result Adapter::Answer(const QString& sessionId) const {
    if (sessionId.isEmpty()) {
        if (!Error_.isEmpty()) {
            return Failed(Error_);
        }
        return QJsonValue(Rows_);
    }
    const auto row = Sessions_.constFind(sessionId);
    if (row == Sessions_.cend()) {
        if (!Error_.isEmpty()) {
            return Failed(Error_);
        }
        return QJsonValue(QJsonValue::Null);
    }
    QJsonObject command{{"program", Executable_},
                        {"args", QJsonArray{"resume", sessionId}}};
    const QString cwd = row->value("cwd").toString();
    if (!cwd.isEmpty()) {
        if (!QDir::isAbsolutePath(cwd) || cwd.contains(QChar::Null) ||
            !QFileInfo(cwd).isDir()) {
            return Failed(
                QStringLiteral("the Codex working directory is unavailable: %1")
                    .arg(cwd));
        }
        command.insert("cwd", cwd);
    }
    return QJsonValue(command);
}

}  // namespace roost::codex
