#include "terminal/session_pane.hpp"

#include <QPointer>
#include <QtLogging>

#include "core/protocol.hpp"

namespace roost {

namespace {

bool IsConnectionLost(const rpc::Error& error) {
    return error.Code == static_cast<int>(protocol::EErrorCode::ConnectionLost);
}

}  // namespace

SessionPane::SessionPane(IoThread& io, QObject* parent)
    : QObject(parent), Io_(io) {
    // The IO thread emits from its own thread; with this as the context the
    // slots below run queued in ours.
    connect(&Io_, &IoThread::Updated, this, &SessionPane::OnUpdated);
    connect(&Io_, &IoThread::Closed, this, &SessionPane::OnClosed);
    connect(&Io_, &IoThread::Disconnected, this, &SessionPane::OnDisconnected);
}

SessionPane::~SessionPane() { CloseChannel(); }

void SessionPane::Show(const std::optional<SessionKey>& key) {
    if (!key) {
        ++Generation_;
        CloseChannel();
        Key_.reset();
        if (Session_) {
            Session_.reset();
            emit SessionChanged();
        }
        SetStatus(EPaneStatus::None);
        return;
    }
    if (key == Key_) {
        Reopen();
        return;
    }
    Key_ = key;
    StartOpen();
}

void SessionPane::Reopen() {
    if (Key_ &&
        (Status_ == EPaneStatus::Ended || Status_ == EPaneStatus::Failed ||
         Status_ == EPaneStatus::Disconnected)) {
        StartOpen();
    }
}

void SessionPane::SetGrid(quint16 columns, quint16 rows) {
    Grid_ = {columns, rows};
    PumpResize();
}

void SessionPane::SendInput(const QByteArray& bytes) {
    if (Channel_ != 0 && Status_ == EPaneStatus::Open) {
        Io_.SendInput(Channel_, bytes);
    }
}

void SessionPane::StartOpen() {
    const auto generation = ++Generation_;
    CloseChannel();
    // The old screen goes at once: the new session shows nothing until it is
    // open.
    if (Session_) {
        Session_.reset();
        emit SessionChanged();
    }
    SetStatus(EPaneStatus::Opening);
    SentSize_ = Grid_;
    // The context is the IO thread, which outlives the pane, so the answer
    // always arrives; the pane may be gone by then.
    Io_.Open(Key_->Agent, Key_->Id, Grid_.first, Grid_.second)
        .then(&Io_, [pane = QPointer(this), io = &Io_,
                     generation](const OpenResult& result) {
            if (pane) {
                pane->OnOpened(generation, result);
            } else if (result) {
                io->Close(result->Channel);
            }
        });
}

void SessionPane::OnOpened(std::uint64_t generation, const OpenResult& result) {
    if (generation != Generation_) {
        if (result) {
            Io_.Close(result->Channel);
        }
        return;
    }
    if (!result) {
        if (IsConnectionLost(result.error())) {
            // OnDisconnected may have said it, with the reason.
            if (Status_ != EPaneStatus::Disconnected) {
                SetStatus(EPaneStatus::Disconnected, result.error().Message);
            }
        } else {
            SetStatus(EPaneStatus::Failed, result.error().Message);
        }
        return;
    }
    Channel_ = result->Channel;
    Session_ = result->Session;
    // Open leaves the channel silent until this first acknowledgement.
    Io_.AcknowledgeUpdate(Channel_);
    // The session first, so a status handler that sees Open already has it.
    emit SessionChanged();
    SetStatus(EPaneStatus::Open);
    emit Updated();
    // The grid may have changed while the open was in flight.
    PumpResize();
}

void SessionPane::OnUpdated(std::uint32_t channel) {
    if (channel != Channel_ || Channel_ == 0) {
        return;
    }
    // Acknowledge before the frame reads the terminal, so output that lands
    // in between raises the signal again.
    Io_.AcknowledgeUpdate(channel);
    emit Updated();
}

void SessionPane::OnClosed(std::uint32_t channel, const QString& reason) {
    if (channel != Channel_ || Channel_ == 0) {
        return;
    }
    // The channel is gone with the session; the last screen stays.
    Channel_ = 0;
    SetStatus(EPaneStatus::Ended, reason);
}

void SessionPane::OnDisconnected(const QString& reason) {
    if (Status_ != EPaneStatus::Open && Status_ != EPaneStatus::Opening) {
        return;
    }
    // The channels went with the connection.
    Channel_ = 0;
    SetStatus(EPaneStatus::Disconnected,
              QStringLiteral("connection to roostd lost: ") + reason);
}

void SessionPane::PumpResize() {
    if (ResizeInFlight_ || Channel_ == 0 || Grid_ == SentSize_) {
        return;
    }
    const auto [columns, rows] = Grid_;
    const auto channel = Channel_;
    SentSize_ = Grid_;
    ResizeInFlight_ = true;
    Io_.Resize(channel, columns, rows)
        .then(this, [this, channel](const rpc::Result& result) {
            ResizeInFlight_ = false;
            if (!result && channel == Channel_) {
                // Not fatal: if the session is gone, Closed or Disconnected
                // follows and sets the real status.
                qWarning("roost: resize failed: %s",
                         qPrintable(result.error().Message));
            }
            PumpResize();
        });
}

void SessionPane::CloseChannel() {
    if (Channel_ != 0) {
        Io_.Close(std::exchange(Channel_, 0));
    }
}

void SessionPane::SetStatus(EPaneStatus status, const QString& message) {
    if (status == Status_ && message == Message_) {
        return;
    }
    Status_ = status;
    Message_ = message;
    emit StatusChanged();
}

}  // namespace roost
