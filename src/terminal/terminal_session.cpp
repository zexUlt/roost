#include "terminal/terminal_session.hpp"

#include <QtLogging>

#include <atomic>
#include <utility>

namespace roost {

namespace {

void Fatal(const char* message) { qFatal("%s", message); }

std::atomic<TerminalSession::CheckHandler>& Handler() {
    static std::atomic<TerminalSession::CheckHandler> handler{&Fatal};
    return handler;
}

}  // namespace

TerminalSession::Locked::Locked(TerminalSession& session)
    : Session_(&session), Lock_(session.Mutex_) {}

TerminalSession::CheckHandler TerminalSession::SetCheckHandler(
    CheckHandler handler) {
    return Handler().exchange(handler != nullptr ? handler : &Fatal);
}

TerminalSession::TerminalSession(Key /*key*/, VtTerminal terminal)
    : Terminal_(std::move(terminal)) {}

TerminalSession::~TerminalSession() = default;

std::expected<std::unique_ptr<TerminalSession>, QString>
TerminalSession::Create(quint16 columns, quint16 rows) {
    auto terminal = VtTerminal::Create(columns, rows);
    if (!terminal) {
        return std::unexpected(terminal.error());
    }
    return std::make_unique<TerminalSession>(Key(), std::move(*terminal));
}

std::expected<std::unique_ptr<TerminalSession>, QString>
TerminalSession::FromSnapshot(QByteArrayView snapshot) {
    auto terminal = VtTerminal::FromSnapshot(snapshot);
    if (!terminal) {
        return std::unexpected(terminal.error());
    }
    return std::make_unique<TerminalSession>(Key(), std::move(*terminal));
}

TerminalSession::Locked TerminalSession::Lock() { return Locked(*this); }

bool TerminalSession::Check(const Locked& lock, const char* method) const {
#ifndef NDEBUG
    if (lock.Session_ != this) {
        Handler().load()(QByteArray("TerminalSession::" + QByteArray(method) +
                                    ": the lock token is of another session")
                             .constData());
        return false;
    }
#else
    Q_UNUSED(lock)
    Q_UNUSED(method)
#endif
    return true;
}

void TerminalSession::Write(const Locked& lock, QByteArrayView bytes) {
    if (!Check(lock, "Write")) {
        return;
    }
    Terminal_.Write(bytes);
}

std::expected<void, QString> TerminalSession::Resize(const Locked& lock,
                                                     quint16 columns,
                                                     quint16 rows) {
    if (!Check(lock, "Resize")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return Terminal_.Resize(columns, rows);
}

std::expected<QByteArray, QString> TerminalSession::Dump(
    const Locked& lock) const {
    if (!Check(lock, "Dump")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return Terminal_.Dump();
}

std::expected<void, QString> TerminalSession::SyncKeyEncoder(
    const Locked& lock, VtKeyEncoder& encoder) const {
    if (!Check(lock, "SyncKeyEncoder")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return encoder.SyncFrom(Terminal_);
}

bool TerminalSession::BracketedPaste(const Locked& lock) const {
    return Check(lock, "BracketedPaste") && Terminal_.BracketedPaste();
}

std::expected<void, QString> TerminalSession::BeginRenderUpdate(
    const Locked& lock) {
    if (!Check(lock, "BeginRenderUpdate")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return RenderState_.BeginUpdate(Terminal_);
}

void TerminalSession::EndRenderUpdate() { RenderState_.EndUpdate(); }

const VtRenderState& TerminalSession::RenderState() const {
    return RenderState_;
}

void TerminalSession::CleanRenderState() { RenderState_.Clean(); }

}  // namespace roost
