#pragma once

#include <QDeadlineTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QPromise>
#include <QSet>
#include <QTimer>

#include <chrono>
#include <functional>
#include <optional>
#include <vector>

#include "common/json_rpc.hpp"

namespace roost::codex {

namespace jsonrpc = plugin::jsonrpc;

class Adapter : public QObject {
public:
    using Send = std::function<void(const QByteArray&)>;

    Adapter(QString program, std::chrono::milliseconds pollInterval,
            std::chrono::milliseconds timeout, Send send);
    ~Adapter() override;

    void Start();
    QFuture<QByteArray> Handle(QByteArrayView message) const;

private:
    struct Waiting {
        QString SessionId;
        QPromise<jsonrpc::Result> Promise;
    };

    void Poll();
    void StartReplyTimer();
    void Read();
    void HandleReply(const QJsonObject& reply);
    void Request(const QString& method, const QJsonObject& params);
    void RequestPage(const QString& cursor = {});
    void HandlePage(const QJsonObject& page);
    void Publish();
    void Fail(const QString& reason);
    void StopProcess();
    void FinishPoll();
    void Notify(const QString& method, const QJsonValue& session) const;
    QFuture<jsonrpc::Result> AfterFirstPoll(const QString& sessionId = {});
    jsonrpc::Result Answer(const QString& sessionId) const;

    QString Program_;
    QString Executable_;
    std::chrono::milliseconds PollInterval_;
    std::chrono::milliseconds Timeout_;
    Send Send_;
    jsonrpc::Dispatcher Dispatcher_;
    QProcess Process_;
    QTimer PollTimer_;
    QTimer TimeoutTimer_;
    QDeadlineTimer PollDeadline_;
    QByteArray Buffer_;
    qint64 NextRequestId_ = 0;
    std::optional<qint64> PendingId_;
    bool Initialized_ = false;
    bool Listed_ = false;
    QString Error_;
    QSet<QString> Cursors_;
    QHash<QString, QJsonObject> PendingSessions_;
    QJsonArray PendingRows_;
    QHash<QString, QJsonObject> Sessions_;
    QJsonArray Rows_;
    std::vector<Waiting> Waiting_;
};

}  // namespace roost::codex
