#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QFuture>
#include <QHash>
#include <QObject>
#include <QPromise>
#include <QSet>
#include <QString>
#include <QTimer>

#include <chrono>
#include <expected>
#include <functional>
#include <optional>
#include <vector>

#include "claude/backend/agents.hpp"
#include "core/rpc.hpp"

class QProcess;

namespace roost::claude {

struct AdapterOptions {
    QString Program = QStringLiteral("claude");
    // From the end of one run of `claude agents` to the start of the next.
    std::chrono::milliseconds PollInterval{3'000};
    // A run that takes longer is killed and counts as failed.
    std::chrono::milliseconds PollTimeout{20'000};
};

// The Claude adapter (design.md §7): lists Claude Code's background sessions
// by running `<Program> agents --json --all`, one run at a time, and answers
// roostd from the last run:
//
//   agent.listSessions {agent: "claude"} -> [{id, state, title, cwd,
//     startedAt?}]
//   agent.attachCommand {agent: "claude", session} -> {program, args} that
//     runs `<Program> attach <session>`, or null for a session it does not
//     list
//
// Requests wait for the first run. While the last run failed, both fail with
// its reason. After every successful run but the first one, each session
// that is new or changed its state since the previous successful run is sent
// as session.stateChanged {agent, session}.
class Adapter : public QObject {
    Q_OBJECT

public:
    static constexpr QLatin1StringView AGENT{"claude"};

    // Sends one compact JSON-RPC message to roostd.
    using Send = std::function<void(const QByteArray&)>;

    Adapter(AdapterOptions options, Send send, QObject* parent = nullptr);
    // Requests still waiting for the first run fail.
    ~Adapter() override;

    Adapter(const Adapter&) = delete;
    Adapter& operator=(const Adapter&) = delete;

    // Starts the first run.
    void Start();

    // One message from roostd. The future yields the response body, or an
    // empty array if none is due.
    QFuture<QByteArray> Handle(QByteArrayView message);

private:
    struct Waiting {
        std::function<rpc::Result()> Answer;
        QPromise<rpc::Result> Promise;
    };

    void Poll();
    void OnPolled(std::expected<Listing, QString> listing);
    // The answer now if a run has finished, else once the first one does.
    QFuture<rpc::Result> WhenPolled(std::function<rpc::Result()> answer);
    rpc::Result ListSessions() const;
    rpc::Result AttachCommand(const QString& sessionId) const;

    AdapterOptions Options_;
    Send Send_;
    rpc::Dispatcher Dispatcher_;

    QProcess* Process_ = nullptr;
    QTimer PollTimer_;
    QTimer TimeoutTimer_;

    // The last run: its sessions or why it failed. Empty before the first.
    std::optional<std::expected<std::vector<Session>, QString>> Last_;
    // States from the last run that succeeded, to tell what changed.
    std::optional<QHash<QString, QString>> Known_;
    std::vector<Waiting> Waiting_;
    // Warnings already logged: the same one comes with every run.
    QSet<QString> Warned_;
};

}  // namespace roost::claude
