#include "terminal/session_pane.hpp"

namespace roost {

SessionPane::SessionPane(WarmTerminals& terminals, QObject* parent)
    : QObject(parent), Terminals_(terminals) {
    // The signals of the pool are for every key; the pane passes on those of
    // its own. They are emitted in this thread, so the slots run at once.
    connect(&Terminals_, &WarmTerminals::SessionChanged, this,
            [this](const SessionKey& key) {
                if (key == Key_) {
                    SyncSession();
                }
            });
    connect(&Terminals_, &WarmTerminals::StatusChanged, this,
            [this](const SessionKey& key) {
                if (key == Key_) {
                    SyncStatus();
                }
            });
    connect(&Terminals_, &WarmTerminals::Updated, this,
            [this](const SessionKey& key) {
                if (key == Key_ && Owns()) {
                    emit Updated();
                }
            });
    connect(&Terminals_, &WarmTerminals::Superseded, this,
            [this](const SessionKey& key) {
                if (key == Key_) {
                    SyncSession();
                    SyncStatus();
                }
            });
}

SessionPane::~SessionPane() {
    if (Key_) {
        Terminals_.Release(*Key_, this);
    }
}

void SessionPane::Show(const std::optional<SessionKey>& key) {
    if (key && key == Key_) {
        Reopen();
        return;
    }
    if (Key_) {
        Terminals_.Release(*Key_, this);
    }
    Key_ = key;
    if (Key_) {
        Terminals_.Acquire(*Key_, this, Grid_.first, Grid_.second);
    }
    // The old screen goes at once, and a warm session's comes in the same
    // call; the pool's own signals have said what changed on the way.
    SyncSession();
    SyncStatus();
}

void SessionPane::Reopen() {
    if (Owns()) {
        Terminals_.Reopen(*Key_);
    }
}

void SessionPane::SetGrid(quint16 columns, quint16 rows) {
    Grid_ = {columns, rows};
    if (Owns()) {
        Terminals_.SetGrid(*Key_, columns, rows);
    }
}

void SessionPane::SendInput(const QByteArray& bytes) {
    if (Owns()) {
        Terminals_.SendInput(*Key_, bytes);
    }
}

std::shared_ptr<TerminalSession> SessionPane::Session() const {
    return Owns() ? Terminals_.Session(*Key_) : nullptr;
}

EPaneStatus SessionPane::Status() const {
    return Owns() ? Terminals_.Status(*Key_) : EPaneStatus::None;
}

QString SessionPane::Message() const {
    return Owns() ? Terminals_.Message(*Key_) : QString();
}

bool SessionPane::Owns() const {
    return Key_ && Terminals_.Owner(*Key_) == this;
}

void SessionPane::SyncSession() {
    auto session = Session();
    if (session != Announced_) {
        Announced_ = std::move(session);
        emit SessionChanged();
    }
}

void SessionPane::SyncStatus() {
    const EPaneStatus status = Status();
    const QString message = Message();
    if (status != AnnouncedStatus_ || message != AnnouncedMessage_) {
        AnnouncedStatus_ = status;
        AnnouncedMessage_ = message;
        emit StatusChanged();
    }
}

}  // namespace roost
