#include "roostd/warm_pool.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace roost {

WarmSession::WarmSession(WarmSessionKey key, VtTerminal terminal,
                         PtyOptions options, QObject* parent)
    : QObject(parent),
      Key_(std::move(key)),
      Terminal_(std::move(terminal)),
      Pty_(new Pty(options, this)) {
    // Only the daemon's terminal answers: the window's copy has no handler.
    Terminal_.SetResponseHandler(
        [pty = Pty_](QByteArrayView response) { pty->Write(response); });
    connect(Pty_, &Pty::Output, this, [this](const QByteArray& bytes) {
        if (Closed_) {
            return;
        }
        Terminal_.Write(bytes);
        emit Output(bytes);
    });
    connect(Pty_, &Pty::Finished, this, [this](const QString& reason) {
        Close(reason);
        deleteLater();
    });
}

PtySize WarmSession::Size() const { return Pty_->Size(); }

qint64 WarmSession::ProcessId() const { return Pty_->ProcessId(); }

std::expected<QByteArray, QString> WarmSession::Snapshot() const {
    return Terminal_.Snapshot();
}

void WarmSession::Write(QByteArrayView bytes) {
    if (!Closed_) {
        Pty_->Write(bytes);
    }
}

std::expected<void, QString> WarmSession::Resize(PtySize size) {
    if (Closed_) {
        return std::unexpected(QStringLiteral("the session is closed"));
    }
    // Check the size first, then the PTY, then the terminal: the terminal
    // fails only on a bad size, so nothing needs rolling back.
    if (size.Columns == 0 || size.Rows == 0 ||
        size.Columns > VtTerminal::MAX_SIZE ||
        size.Rows > VtTerminal::MAX_SIZE) {
        return std::unexpected(
            QStringLiteral("columns and rows are from 1 to %1")
                .arg(VtTerminal::MAX_SIZE));
    }
    if (!Pty_->Resize(size)) {
        return std::unexpected(QStringLiteral("the PTY is closed"));
    }
    if (auto resized = Terminal_.Resize(size.Columns, size.Rows); !resized) {
        return resized;
    }
    return {};
}

void WarmSession::Close(const QString& reason) {
    if (Closed_) {
        return;
    }
    Closed_ = true;
    emit Closed(reason);
}

void WarmSession::Evict(const QString& reason) {
    Close(reason);
    // Finished comes once the program has gone, at most a hang-up timeout
    // from now, and deletes the session.
    Pty_->HangUp();
}

WarmPool::WarmPool(WarmPoolOptions options, QObject* parent)
    : QObject(parent), Options_(options) {
    Options_.Capacity = std::max<qsizetype>(Options_.Capacity, 1);
}

std::expected<WarmSession*, QString> WarmPool::Open(const WarmSessionKey& key,
                                                    const PtySpec& spec,
                                                    PtySize size) {
    if (WarmSession* warm = Find(key); warm != nullptr) {
        Touch(warm);
        return warm;
    }
    auto terminal = VtTerminal::Create(size.Columns, size.Rows);
    if (!terminal) {
        return std::unexpected(terminal.error());
    }
    auto* session =
        new WarmSession(key, std::move(*terminal), Options_.Pty, this);
    if (auto started = session->Pty_->Start(spec, size); !started) {
        delete session;
        return std::unexpected(started.error());
    }
    connect(session, &WarmSession::Closed, this,
            [this, session] { Forget(session); });
    Sessions_.insert(key, session);
    Touch(session);
    EvictExtra();
    return session;
}

WarmSession* WarmPool::Find(const WarmSessionKey& key) const {
    return Sessions_.value(key, nullptr);
}

void WarmPool::Touch(WarmSession* session) { session->LastViewed_ = ++Clock_; }

std::vector<WarmSessionKey> WarmPool::KeysMostRecentFirst() const {
    std::vector<const WarmSession*> sessions;
    sessions.reserve(static_cast<size_t>(Sessions_.size()));
    for (const WarmSession* session : Sessions_) {
        sessions.push_back(session);
    }
    std::ranges::sort(
        sessions, std::greater<>(),
        [](const WarmSession* session) { return session->LastViewed_; });
    std::vector<WarmSessionKey> keys;
    keys.reserve(sessions.size());
    for (const WarmSession* session : sessions) {
        keys.push_back(session->Key());
    }
    return keys;
}

void WarmPool::Forget(WarmSession* session) {
    // An evicted session is forgotten before its program has gone, and the
    // key may be warm again by then.
    if (Find(session->Key()) == session) {
        Sessions_.remove(session->Key());
    }
}

void WarmPool::Evict(const WarmSessionKey& key, const QString& reason) {
    if (WarmSession* session = Find(key); session != nullptr) {
        // Closed forgets it.
        session->Evict(reason);
    }
}

void WarmPool::EvictExtra() {
    while (Sessions_.size() > Options_.Capacity) {
        const auto oldest = std::ranges::min_element(
            Sessions_, {},
            [](const WarmSession* session) { return session->LastViewed_; });
        // Closed forgets it.
        (*oldest)->Evict(QStringLiteral("evicted"));
    }
}

}  // namespace roost
