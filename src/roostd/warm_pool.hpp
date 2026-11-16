#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QHash>
#include <QHashFunctions>
#include <QObject>
#include <QString>

#include <expected>

#include "core/pty.hpp"
#include "core/vt_terminal.hpp"

namespace roost {

// A session of an agent: the agent and its id for the session.
struct WarmSessionKey {
    QString Agent;
    QString Session;

    bool operator==(const WarmSessionKey&) const = default;
};

inline size_t qHash(const WarmSessionKey& key, size_t seed = 0) noexcept {
    return qHashMulti(seed, key.Agent, key.Session);
}

// What the daemon keeps of a warm session (design.md §3): the attach command
// in a PTY and a terminal that its output goes through. The terminal answers
// the program's queries, whether a window watches or not.
//
// Closed is emitted once: when the program has exited, or when the pool has
// evicted the session. Nothing is emitted after it. The pool owns and deletes
// the session; signals run inside its code, so a slot must not delete it.
class WarmSession : public QObject {
    Q_OBJECT

public:
    const WarmSessionKey& Key() const { return Key_; }
    PtySize Size() const;
    qint64 ProcessId() const;
    bool IsClosed() const { return Closed_; }

    // The terminal as the output emitted so far has left it.
    std::expected<QByteArray, QString> Snapshot() const;
    // Dropped once the session is closed.
    void Write(QByteArrayView bytes);
    // The PTY and the terminal both; sizes as for VtTerminal::Create.
    std::expected<void, QString> Resize(PtySize size);

signals:
    // Emitted after the terminal has taken the bytes.
    void Output(const QByteArray& bytes);
    // "evicted", or the PTY's reason, "exited with code N" and the like.
    void Closed(const QString& reason);

private:
    friend class WarmPool;

    WarmSession(WarmSessionKey key, VtTerminal terminal, PtyOptions options,
                QObject* parent);

    void Close(const QString& reason);
    // Closes the session and hangs the program up; the session is deleted
    // once the program has gone.
    void Evict();

    WarmSessionKey Key_;
    VtTerminal Terminal_;
    Pty* Pty_;
    bool Closed_ = false;
    quint64 LastViewed_ = 0;
};

struct WarmPoolOptions {
    // How many sessions stay warm; at least 1.
    qsizetype Capacity = 4;
    PtyOptions Pty;
};

// The warm sessions of the daemon (design.md §3), at most Capacity of them.
// When one more is opened, the one viewed least recently is evicted: its
// program is hung up and its session closed.
//
// An evicted session leaves the pool at once and does not count in Capacity,
// but its program lives until it exits on SIGHUP, at most a hang-up timeout:
// for a moment more than Capacity programs may be alive.
//
// Destroying the pool kills the programs without emitting Closed.
class WarmPool : public QObject {
    Q_OBJECT

public:
    explicit WarmPool(WarmPoolOptions options = {}, QObject* parent = nullptr);

    // The warm session of the key, if there is one, else a new one with the
    // program of the spec, in a PTY and a terminal of the size. Either way the
    // session counts as viewed now. A warm session keeps its size: the caller
    // resizes it if it needs to. Nothing is evicted if the program fails to
    // start. Do not call it from a slot of WarmSession::Output: an eviction
    // would close a PTY inside its own read.
    std::expected<WarmSession*, QString> Open(const WarmSessionKey& key,
                                              const PtySpec& spec,
                                              PtySize size);
    // Null if the session is not warm.
    WarmSession* Find(const WarmSessionKey& key) const;
    // The session counts as viewed now.
    void Touch(WarmSession* session);
    qsizetype Count() const { return Sessions_.size(); }

private:
    void Forget(WarmSession* session);
    void EvictExtra();

    WarmPoolOptions Options_;
    QHash<WarmSessionKey, WarmSession*> Sessions_;
    quint64 Clock_ = 0;
};

}  // namespace roost
