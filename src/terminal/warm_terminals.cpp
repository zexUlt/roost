#include "terminal/warm_terminals.hpp"

#include <QJsonObject>
#include <QPointer>
#include <QtLogging>

#include <algorithm>

#include "core/protocol.hpp"
#include "core/rpc.hpp"

namespace roost {

namespace {

bool IsConnectionLost(const rpc::Error& error) {
    return error.Code == static_cast<int>(protocol::EErrorCode::ConnectionLost);
}

bool IsMethodNotFound(const rpc::Error& error) {
    return error.Code == static_cast<int>(rpc::EErrorCode::MethodNotFound);
}

}  // namespace

WarmTerminals::WarmTerminals(IoThread& io, QObject* parent)
    : QObject(parent), Io_(io) {
    // The IO thread emits from its own thread; with this as the context the
    // slots below run queued in ours.
    connect(&Io_, &IoThread::Updated, this, &WarmTerminals::OnUpdated);
    connect(&Io_, &IoThread::Closed, this, &WarmTerminals::OnClosed);
    connect(&Io_, &IoThread::Disconnected, this,
            &WarmTerminals::OnDisconnected);
}

WarmTerminals::~WarmTerminals() {
    for (const auto& entry : Entries_) {
        if (entry->Channel != 0) {
            Io_.Close(entry->Channel);
        }
    }
}

void WarmTerminals::Acquire(const SessionKey& key, const QObject* owner,
                            quint16 columns, quint16 rows) {
    if (Entry* const warm = Find(key)) {
        const QObject* const previous = warm->Owner;
        warm->Shown = true;
        warm->Owner = owner;
        warm->Grid = {columns, rows};
        // A hidden entry is Open or Opening. An open in flight is
        // acknowledged when its answer comes; a live one switches in here:
        // its session and status are already what the pane shows.
        if (warm->Status == EPaneStatus::Open) {
            // Before the frame reads the terminal; output that lands in
            // between raises Updated again.
            Io_.AcknowledgeUpdate(warm->Channel);
            SendView(warm->Channel);
            PumpResize(*warm);
        }
        if (previous != nullptr && previous != owner) {
            emit Superseded(SessionKey(key));
        }
        return;
    }
    auto& entry = *Entries_.emplace_back(std::make_unique<Entry>());
    entry.Key = key;
    entry.Shown = true;
    entry.Owner = owner;
    entry.Grid = {columns, rows};
    StartOpen(key);
}

void WarmTerminals::Release(const SessionKey& key, const QObject* owner) {
    Entry* const entry = Find(key);
    if (entry == nullptr || entry->Owner != owner) {
        return;
    }
    entry->Shown = false;
    entry->Owner = nullptr;
    // What is not live or coming has no channel to keep warm.
    if (entry->Status != EPaneStatus::Open &&
        entry->Status != EPaneStatus::Opening) {
        Erase(entry);
    }
}

void WarmTerminals::SetGrid(const SessionKey& key, quint16 columns,
                            quint16 rows) {
    if (Entry* const entry = Find(key)) {
        entry->Grid = {columns, rows};
        PumpResize(*entry);
    }
}

void WarmTerminals::SendInput(const SessionKey& key, const QByteArray& bytes) {
    const Entry* const entry = Find(key);
    if (entry != nullptr && entry->Channel != 0 &&
        entry->Status == EPaneStatus::Open) {
        Io_.SendInput(entry->Channel, bytes);
    }
}

void WarmTerminals::Reopen(const SessionKey& key) {
    Entry* const entry = Find(key);
    if (entry != nullptr && entry->Shown &&
        (entry->Status == EPaneStatus::Ended ||
         entry->Status == EPaneStatus::Failed ||
         entry->Status == EPaneStatus::Disconnected)) {
        StartOpen(key);
    }
}

void WarmTerminals::ReopenShown() {
    // StartOpen emits, and a slot may change the entries: work from the keys.
    std::vector<SessionKey> keys;
    for (const auto& entry : Entries_) {
        if (entry->Shown && entry->Status == EPaneStatus::Disconnected) {
            keys.push_back(entry->Key);
        }
    }
    for (const SessionKey& key : keys) {
        Reopen(key);
    }
}

EPaneStatus WarmTerminals::Status(const SessionKey& key) const {
    const Entry* const entry = Find(key);
    return entry != nullptr ? entry->Status : EPaneStatus::None;
}

QString WarmTerminals::Message(const SessionKey& key) const {
    const Entry* const entry = Find(key);
    return entry != nullptr ? entry->Message : QString();
}

std::shared_ptr<TerminalSession> WarmTerminals::Session(
    const SessionKey& key) const {
    const Entry* const entry = Find(key);
    return entry != nullptr ? entry->Session : nullptr;
}

std::uint32_t WarmTerminals::Channel(const SessionKey& key) const {
    const Entry* const entry = Find(key);
    return entry != nullptr ? entry->Channel : 0;
}

bool WarmTerminals::IsShown(const SessionKey& key) const {
    const Entry* const entry = Find(key);
    return entry != nullptr && entry->Shown;
}

const QObject* WarmTerminals::Owner(const SessionKey& key) const {
    const Entry* const entry = Find(key);
    return entry != nullptr ? entry->Owner : nullptr;
}

WarmTerminals::Entry* WarmTerminals::Find(const SessionKey& key) const {
    const auto found = std::ranges::find_if(
        Entries_, [&key](const auto& entry) { return entry->Key == key; });
    return found != Entries_.end() ? found->get() : nullptr;
}

WarmTerminals::Entry* WarmTerminals::FindByChannel(
    std::uint32_t channel) const {
    if (channel == 0) {
        return nullptr;
    }
    const auto found = std::ranges::find_if(
        Entries_,
        [channel](const auto& entry) { return entry->Channel == channel; });
    return found != Entries_.end() ? found->get() : nullptr;
}

void WarmTerminals::Erase(const Entry* entry) {
    std::erase_if(Entries_,
                  [entry](const auto& owned) { return owned.get() == entry; });
}

void WarmTerminals::StartOpen(const SessionKey& key) {
    Entry* entry = Find(key);
    if (entry == nullptr) {
        return;
    }
    const std::uint64_t generation = ++LastGeneration_;
    entry->Generation = generation;
    entry->ResizeInFlight = false;
    // A slot of an emit below may release the entry or open it again.
    const auto stale = [&] {
        entry = Find(key);
        return entry == nullptr || entry->Generation != generation;
    };
    // The old screen goes at once: the session shows nothing until it is
    // open.
    if (entry->Session) {
        entry->Session.reset();
        emit SessionChanged(SessionKey(key));
        if (stale()) {
            return;
        }
    }
    SetStatus(*entry, EPaneStatus::Opening);
    if (stale()) {
        return;
    }
    entry->SentSize = entry->Grid;
    // The context is the IO thread, which outlives the pool, so the answer
    // always arrives; the pool may be gone by then.
    Io_.Open(key.Agent, key.Id, entry->Grid.first, entry->Grid.second)
        .then(&Io_, [pool = QPointer(this), io = &Io_, key,
                     generation](const OpenResult& result) {
            if (pool) {
                pool->OnOpened(key, generation, result);
            } else if (result) {
                io->Close(result->Channel);
            }
        });
}

void WarmTerminals::OnOpened(const SessionKey& key, std::uint64_t generation,
                             const OpenResult& result) {
    Entry* const entry = Find(key);
    if (entry == nullptr || entry->Generation != generation) {
        // Dropped or opened again since: nobody wants this channel.
        if (result) {
            Io_.Close(result->Channel);
        }
        return;
    }
    if (!result) {
        if (!entry->Shown) {
            Erase(entry);
        } else if (IsConnectionLost(result.error())) {
            // OnDisconnected may have said it, with the reason.
            if (entry->Status != EPaneStatus::Disconnected) {
                SetStatus(*entry, EPaneStatus::Disconnected,
                          result.error().Message);
            }
        } else {
            SetStatus(*entry, EPaneStatus::Failed, result.error().Message);
        }
        return;
    }
    entry->Channel = result->Channel;
    entry->Session = result->Session;
    if (entry->Shown) {
        // Open leaves the channel silent until this first acknowledgement.
        Io_.AcknowledgeUpdate(entry->Channel);
    }
    // The session first, so a status handler that sees Open already has it.
    emit SessionChanged(SessionKey(key));
    SetStatus(*entry, EPaneStatus::Open);
    if (Entry* const now = Find(key); now != nullptr && now->Shown) {
        emit Updated(SessionKey(key));
        // The grid may have changed while the open was in flight.
        PumpResize(*now);
    }
}

void WarmTerminals::OnUpdated(std::uint32_t channel) {
    const Entry* const entry = FindByChannel(channel);
    // A hidden session is not acknowledged: it raises no more Updated until it
    // is shown again, and the terminal is read only then.
    if (entry == nullptr || !entry->Shown) {
        return;
    }
    // Acknowledge before the frame reads the terminal, so output that lands
    // in between raises the signal again.
    Io_.AcknowledgeUpdate(channel);
    emit Updated(SessionKey(entry->Key));
}

void WarmTerminals::OnClosed(std::uint32_t channel, const QString& reason) {
    Entry* const entry = FindByChannel(channel);
    if (entry == nullptr) {
        return;
    }
    if (!entry->Shown) {
        Erase(entry);
        return;
    }
    // The channel is gone with the session; the last screen stays.
    entry->Channel = 0;
    SetStatus(*entry, EPaneStatus::Ended, reason);
}

void WarmTerminals::OnDisconnected(const QString& reason) {
    // The channels went with the connection, and a hidden session cannot be
    // shown without an open.
    std::erase_if(Entries_, [](const auto& entry) { return !entry->Shown; });
    // A shown entry may be changed by the slots of SetStatus: work from keys.
    std::vector<SessionKey> keys;
    for (const auto& entry : Entries_) {
        keys.push_back(entry->Key);
    }
    for (const SessionKey& key : keys) {
        Entry* const entry = Find(key);
        if (entry == nullptr) {
            continue;
        }
        entry->Channel = 0;
        if (entry->Status == EPaneStatus::Open ||
            entry->Status == EPaneStatus::Opening) {
            SetStatus(*entry, EPaneStatus::Disconnected,
                      QStringLiteral("connection to roostd lost: ") + reason);
        }
    }
}

void WarmTerminals::SendView(std::uint32_t channel) {
    Io_.Request(QStringLiteral("session.view"),
                QJsonObject{
                    {QStringLiteral("channel"), static_cast<qint64>(channel)}})
        .then(this, [](const rpc::Result& result) {
            // An old roostd does not know the method; its pool then ranks the
            // session by what it last did, not by what was viewed.
            if (!result && !IsMethodNotFound(result.error()) &&
                !IsConnectionLost(result.error())) {
                qWarning("roost: session.view failed: %s",
                         qPrintable(result.error().Message));
            }
        });
}

void WarmTerminals::PumpResize(Entry& entry) {
    // A hidden session keeps the size it has until it is shown.
    if (!entry.Shown || entry.ResizeInFlight || entry.Channel == 0 ||
        entry.Grid == entry.SentSize) {
        return;
    }
    const auto [columns, rows] = entry.Grid;
    const auto channel = entry.Channel;
    entry.SentSize = entry.Grid;
    entry.ResizeInFlight = true;
    Io_.Resize(channel, columns, rows)
        .then(this,
              [this, key = entry.Key, channel](const rpc::Result& result) {
                  Entry* const now = Find(key);
                  if (now == nullptr || now->Channel != channel) {
                      return;
                  }
                  now->ResizeInFlight = false;
                  if (!result) {
                      // Not fatal: if the session is gone, Closed or
                      // Disconnected follows and sets the real status.
                      qWarning("roost: resize failed: %s",
                               qPrintable(result.error().Message));
                  }
                  PumpResize(*now);
              });
}

void WarmTerminals::SetStatus(Entry& entry, EPaneStatus status,
                              const QString& message) {
    if (status == entry.Status && message == entry.Message) {
        return;
    }
    entry.Status = status;
    entry.Message = message;
    emit StatusChanged(SessionKey(entry.Key));
}

}  // namespace roost
