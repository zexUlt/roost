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

std::expected<void, QString> TerminalSession::SyncMouseEncoder(
    const Locked& lock, VtMouseEncoder& encoder,
    const MouseGeometry& geometry) const {
    if (!Check(lock, "SyncMouseEncoder")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return encoder.SyncFrom(Terminal_, geometry);
}

std::expected<VtTerminal::WheelModes, QString> TerminalSession::ReadWheelModes(
    const Locked& lock) const {
    if (!Check(lock, "ReadWheelModes")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return Terminal_.ReadWheelModes();
}

std::expected<void, QString> TerminalSession::ScrollViewport(const Locked& lock,
                                                             qint64 rows) {
    if (!Check(lock, "ScrollViewport")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return Terminal_.ScrollViewport(rows);
}

std::expected<void, QString> TerminalSession::ScrollViewportToBottom(
    const Locked& lock) {
    if (!Check(lock, "ScrollViewportToBottom")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return Terminal_.ScrollViewportToBottom();
}

std::expected<bool, QString> TerminalSession::ViewportAtBottom(
    const Locked& lock) const {
    if (!Check(lock, "ViewportAtBottom")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return Terminal_.ViewportAtBottom();
}

bool TerminalSession::BracketedPaste(const Locked& lock) const {
    return Check(lock, "BracketedPaste") && Terminal_.BracketedPaste();
}

std::expected<void, QString> TerminalSession::SelectionPress(
    const Locked& lock, const VtTerminal::SelectionPointer& pointer,
    const MouseGeometry& geometry, const VtTerminal::SelectionClick& click) {
    if (!Check(lock, "SelectionPress")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return Terminal_.SelectionPress(pointer, geometry, click);
}

std::expected<void, QString> TerminalSession::SelectionDrag(
    const Locked& lock, const VtTerminal::SelectionPointer& pointer,
    const MouseGeometry& geometry) {
    if (!Check(lock, "SelectionDrag")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return Terminal_.SelectionDrag(pointer, geometry);
}

std::expected<void, QString> TerminalSession::SelectionRelease(
    const Locked& lock, const VtTerminal::SelectionPointer& pointer,
    const MouseGeometry& geometry) {
    if (!Check(lock, "SelectionRelease")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return Terminal_.SelectionRelease(pointer, geometry);
}

std::expected<void, QString> TerminalSession::SelectionAutoscrollTick(
    const Locked& lock, const VtTerminal::SelectionPointer& pointer,
    const MouseGeometry& geometry) {
    if (!Check(lock, "SelectionAutoscrollTick")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return Terminal_.SelectionAutoscrollTick(pointer, geometry);
}

std::expected<VtTerminal::ESelectionAutoscroll, QString>
TerminalSession::SelectionAutoscroll(const Locked& lock) const {
    if (!Check(lock, "SelectionAutoscroll")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return Terminal_.SelectionAutoscroll();
}

std::expected<void, QString> TerminalSession::ResetSelectionGesture(
    const Locked& lock) {
    if (!Check(lock, "ResetSelectionGesture")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return Terminal_.ResetSelectionGesture();
}

std::expected<void, QString> TerminalSession::ClearSelection(
    const Locked& lock) {
    if (!Check(lock, "ClearSelection")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return Terminal_.ClearSelection();
}

std::expected<bool, QString> TerminalSession::HasSelection(
    const Locked& lock) const {
    if (!Check(lock, "HasSelection")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return Terminal_.HasSelection();
}

std::expected<QString, QString> TerminalSession::SelectedText(
    const Locked& lock) const {
    if (!Check(lock, "SelectedText")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    return Terminal_.SelectedText();
}

std::expected<void, QString> TerminalSession::BeginRenderUpdate(
    const Locked& lock) {
    if (!Check(lock, "BeginRenderUpdate")) {
        return std::unexpected(QStringLiteral("wrong lock token"));
    }
    SelectionAtLastRender_ = Terminal_.HasSelection().value_or(false);
    return RenderState_.BeginUpdate(Terminal_);
}

void TerminalSession::EndRenderUpdate() { RenderState_.EndUpdate(); }

const VtRenderState& TerminalSession::RenderState() const {
    return RenderState_;
}

void TerminalSession::CleanRenderState() { RenderState_.Clean(); }

}  // namespace roost
