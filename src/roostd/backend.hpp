#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QFuture>
#include <QJsonValue>
#include <QObject>
#include <QPromise>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <unordered_map>

#include "core/rpc.hpp"

class QProcess;

namespace roost {

struct BackendOptions {
    std::chrono::milliseconds FirstRestartDelay{500};
    std::chrono::milliseconds MaxRestartDelay{30'000};
    // Crashes in a row before the backend is left down.
    int MaxRestarts = 5;
    // A run at least this long ends a row of crashes.
    std::chrono::milliseconds StableRun{60'000};
    // How long the backend has to exit after its stdin closes.
    std::chrono::milliseconds StopTimeout{1'000};
};

// A plugin backend (design.md §5): a child process that speaks JSON-RPC 2.0
// over stdio, one compact message per line. roostd sends requests and the
// backend answers them; the backend's notifications come out as Notified, and
// a request from it is answered with MethodNotFound until roostd serves
// backends methods. Its stderr goes to roostd's. Its stdin closing means it
// should exit.
//
// A backend that exits on its own, fails to start or breaks the protocol has
// crashed: its pending requests fail with BackendUnavailable, Crashed is
// emitted and it starts again after a delay that doubles with each crash in
// a row. After MaxRestarts crashes in a row it stays down and GaveUp is
// emitted instead.
//
// Signals and continuations of requests run inside the backend's own code: a
// Backend is not deleted from them, only through deleteLater().
class Backend : public QObject {
    Q_OBJECT

public:
    static constexpr qsizetype MAX_LINE_LENGTH = 16LL * 1024 * 1024;

    // command: the program, then its arguments.
    Backend(QString name, QStringList command, QString workingDirectory,
            BackendOptions options = {}, QObject* parent = nullptr);
    // Closes stdin, waits up to StopTimeout for the process, then kills it.
    ~Backend() override;

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    void Start();

    // Fails at once with BackendUnavailable while the backend is down.
    // There is no timeout.
    QFuture<rpc::Result> Request(
        const QString& method,
        const QJsonValue& params = QJsonValue::Undefined);

    bool IsRunning() const;
    qint64 ProcessId() const;

signals:
    void Started();
    void Crashed(const QString& reason, qint64 restartInMs);
    void GaveUp(const QString& reason);
    // params is Undefined if the notification has none.
    void Notified(const QString& method, const QJsonValue& params);

private:
    void Launch();
    void Read();
    void Dispatch(const QByteArray& line);
    void Answer(const QJsonValue& id);
    // Kills the process; its exit then counts as a crash for this reason.
    void Break(const QString& reason);
    void OnStopped(const QString& reason);
    void FailPending(const QString& reason);

    QString Name_;
    QStringList Command_;
    QString WorkingDirectory_;
    BackendOptions Options_;

    QProcess* Process_ = nullptr;
    QTimer RestartTimer_;
    QElapsedTimer Uptime_;
    QByteArray Buffer_;
    QString BreakReason_;
    int CrashesInARow_ = 0;
    bool Stopping_ = false;

    std::unordered_map<qint64, QPromise<rpc::Result>> Pending_;
    qint64 NextId_ = 1;
};

}  // namespace roost
