#include "roost/single_session.hpp"

namespace roost {

SingleSession::SingleSession(TerminalItem* item, QString agent, QString session,
                             QObject* parent)
    : QObject(parent),
      Item_(item),
      Agent_(std::move(agent)),
      SessionId_(std::move(session)) {
    // The IO thread emits from its own thread; with this as the context the
    // slots below run queued in ours.
    connect(&Io_, &IoThread::Updated, this, &SingleSession::OnUpdated);
    connect(&Io_, &IoThread::Closed, this, &SingleSession::OnClosed);
    connect(&Io_, &IoThread::Disconnected, this, [this](const QString& reason) {
        if (!Finished()) {
            Fail(QStringLiteral("connection to roostd lost: ") + reason);
        }
    });
    if (Item_ == nullptr) {
        return;
    }
    connect(Item_, &TerminalItem::input, this, [this](const QByteArray& bytes) {
        if (Opened_.Channel != 0 && !Finished()) {
            Io_.SendInput(Opened_.Channel, bytes);
        }
    });
    connect(Item_, &TerminalItem::gridSizeChanged, this, [this] {
        WantedSize_ = std::pair{static_cast<quint16>(Item_->columns()),
                                static_cast<quint16>(Item_->rows())};
        PumpResize();
    });
}

SingleSession::~SingleSession() { Io_.Stop(); }

std::shared_ptr<TerminalSession> SingleSession::Session() const {
    return Opened_.Session;
}

void SingleSession::Start(const QString& socketPath) {
    if (Item_ == nullptr) {
        Fail(QStringLiteral("no terminal item to show the session in"));
        return;
    }
    Io_.Connect(socketPath).then(this, [this](const rpc::Result& result) {
        if (!result) {
            Fail(QStringLiteral("cannot connect to roostd: ") +
                 result.error().Message);
            return;
        }
        if (Item_ == nullptr) {
            Fail(QStringLiteral("the terminal item is gone"));
            return;
        }
        const auto columns = static_cast<quint16>(Item_->columns());
        const auto rows = static_cast<quint16>(Item_->rows());
        SentSize_ = std::pair{columns, rows};
        Io_.Open(Agent_, SessionId_, columns, rows)
            .then(this, [this](const OpenResult& opened) { OnOpened(opened); });
    });
}

void SingleSession::OnOpened(const OpenResult& result) {
    if (Finished()) {
        return;
    }
    if (!result) {
        Fail(QStringLiteral("cannot open session %1 of %2: %3")
                 .arg(SessionId_, Agent_, result.error().Message));
        return;
    }
    Opened_ = *result;
    if (Item_ == nullptr) {
        Fail(QStringLiteral("the terminal item is gone"));
        return;
    }
    Item_->setSession(Opened_.Session);
    // Open leaves the channel silent until this first acknowledgement.
    Io_.AcknowledgeUpdate(Opened_.Channel);
    Item_->update();
    // The item may have changed size while the open was in flight.
    WantedSize_ = std::pair{static_cast<quint16>(Item_->columns()),
                            static_cast<quint16>(Item_->rows())};
    PumpResize();
    qInfo("roost: session %s of %s opened", qPrintable(SessionId_),
          qPrintable(Agent_));
    emit Opened();
}

void SingleSession::OnUpdated(std::uint32_t channel) {
    if (channel != Opened_.Channel || Finished() || Item_ == nullptr) {
        return;
    }
    // Acknowledge before the frame reads the terminal, so output that lands
    // in between raises the signal again.
    Io_.AcknowledgeUpdate(channel);
    Item_->update();
}

void SingleSession::OnClosed(std::uint32_t channel, const QString& reason) {
    if (channel != Opened_.Channel || Finished()) {
        return;
    }
    Done_ = true;
    emit Ended(reason);
}

void SingleSession::PumpResize() {
    if (ResizeInFlight_ || Opened_.Channel == 0 || Finished() || !WantedSize_ ||
        WantedSize_ == SentSize_) {
        return;
    }
    const auto [columns, rows] = *WantedSize_;
    SentSize_ = WantedSize_;
    ResizeInFlight_ = true;
    Io_.Resize(Opened_.Channel, columns, rows)
        .then(this, [this](const rpc::Result& result) {
            ResizeInFlight_ = false;
            if (!result) {
                // Not fatal: if the session is gone, Closed or Disconnected
                // follows and ends the controller with the real reason.
                qCritical("roost: resize failed: %s",
                          qPrintable(result.error().Message));
            }
            PumpResize();
        });
}

void SingleSession::Fail(const QString& message) {
    if (Done_) {
        return;
    }
    Done_ = true;
    qCritical("roost: %s", qPrintable(message));
    emit Failed(message);
}

}  // namespace roost
