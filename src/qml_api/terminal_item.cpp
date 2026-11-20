#include "qml_api/terminal_item.hpp"

#include "api/api.hpp"
#include "core/vt_terminal.hpp"
#include "qml_api/roost_singleton.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QJSValue>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QStyleHints>
#include <QWheelEvent>
#include <QtLogging>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace roost {

namespace {

// What the window logs about its terminal, to debug a switch of sessions: ids
// and agent names, statuses and the order of events. Never terminal output,
// input or the environment. Debug lines are off unless asked for
// (QT_LOGGING_RULES="roost.terminal.debug=true").
Q_LOGGING_CATEGORY(TerminalCategory, "roost.terminal", QtInfoMsg)

// Rows a notch of a wheel scrolls; Ghostty's default for a wheel that is not
// a trackpad. Qt reports a notch as 120 units.
constexpr qreal ROWS_PER_NOTCH = 3;
constexpr qreal UNITS_PER_NOTCH = 120;

// Ghostty's selection scroll runs on a 15 ms timer.
constexpr int AUTOSCROLL_INTERVAL_MS = 15;

// The whole units in `pending` plus `delta`; what is left stays in `pending`.
// Truncates toward zero, so the remainder keeps the sign of the movement.
int takeWhole(qreal& pending, qreal delta, int unit) {
    if (unit <= 0) {
        return 0;
    }
    pending += delta;
    const auto whole = static_cast<int>(pending / unit);
    pending -= static_cast<qreal>(whole) * unit;
    return whole;
}

// The button a motion is reported with: the first one held.
EMouseButton heldButton(Qt::MouseButtons buttons) {
    for (const auto button : {Qt::LeftButton, Qt::MiddleButton, Qt::RightButton,
                              Qt::BackButton, Qt::ForwardButton}) {
        if (buttons.testFlag(button)) {
            return MapQtMouseButton(button);
        }
    }
    return EMouseButton::None;
}

// The text of a status, as QML reads it.
QString statusName(EPaneStatus status) {
    switch (status) {
        case EPaneStatus::None:
            return QStringLiteral("none");
        case EPaneStatus::Opening:
            return QStringLiteral("opening");
        case EPaneStatus::Open:
            return QStringLiteral("open");
        case EPaneStatus::Ended:
            return QStringLiteral("ended");
        case EPaneStatus::Failed:
            return QStringLiteral("failed");
        case EPaneStatus::Disconnected:
            return QStringLiteral("disconnected");
    }
    return QStringLiteral("none");
}

// A JS object arrives as a QVariantMap, null and undefined as an invalid or
// null variant, and a value of type var may come wrapped as a QJSValue.
bool isNullRef(const QVariant& value) {
    return !value.isValid() || value.isNull() ||
           value.metaType() == QMetaType::fromType<std::nullptr_t>();
}

// The agent or id of a ref: a non-empty string, or empty if it is not.
QString refField(const QVariantMap& ref, const QString& name) {
    const QVariant value = ref.value(name);
    return value.metaType().id() == QMetaType::QString ? value.toString()
                                                       : QString();
}

}  // namespace

TerminalItem::TerminalItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
    setActiveFocusOnTab(true);
    setAcceptedMouseButtons(Qt::AllButtons);
    // Motion with no button held is a report in any-event mode.
    setAcceptHoverEvents(true);
    if (auto encoder = VtKeyEncoder::Create()) {
        KeyEncoder_.emplace(std::move(*encoder));
        KeyEncoder_->SetMacosOptionAsAlt(KeyOptions_.OptionAsAlt);
    } else {
        qWarning("TerminalItem: %s", qPrintable(encoder.error()));
    }
    if (auto encoder = VtMouseEncoder::Create()) {
        MouseEncoder_.emplace(std::move(*encoder));
    } else {
        qWarning("TerminalItem: %s", qPrintable(encoder.error()));
    }
    AutoscrollTimer_.setInterval(AUTOSCROLL_INTERVAL_MS);
    connect(&AutoscrollTimer_, &QTimer::timeout, this,
            &TerminalItem::autoscrollTick);
}

QString TerminalItem::logLabel(bool hasSession) const {
    if (!hasSession) {
        return QStringLiteral("none");
    }
    if (Pane_ && Pane_->Key()) {
        return Pane_->Key()->Agent + u'/' + Pane_->Key()->Id;
    }
    if (SessionRef_.isValid()) {
        const QVariantMap ref = SessionRef_.toMap();
        return ref.value(QStringLiteral("agent")).toString() + u'/' +
               ref.value(QStringLiteral("id")).toString();
    }
    return QStringLiteral("unkeyed");
}

void TerminalItem::setSession(std::shared_ptr<TerminalSession> session) {
    const QString label = logLabel(session != nullptr);
    qCInfo(TerminalCategory, "setSession %s -> %s", qPrintable(LoggedSession_),
           qPrintable(label));
    LoggedSession_ = label;
    PaintLogPending_ = session != nullptr;
    // The old gesture is not the new session's.
    endDrag();
    Session_ = std::move(session);
    syncHasSelection();
    // The old session's rows are still in the scene; a clean session on the
    // same grid would leave them there.
    Renderer_.Invalidate();
    update();
}

void TerminalItem::setSessionRef(const QVariant& ref) {
    QVariant value = ref;
    if (value.metaType() == QMetaType::fromType<QJSValue>()) {
        value = value.value<QJSValue>().toVariant();
    }
    QVariant accepted;
    bool invalid = false;
    if (!isNullRef(value)) {
        const QVariantMap map = value.toMap();
        const QString agent = refField(map, QStringLiteral("agent"));
        const QString id = refField(map, QStringLiteral("id"));
        if (agent.isEmpty() || id.isEmpty()) {
            invalid = true;
        } else {
            accepted = QVariantMap{{QStringLiteral("agent"), agent},
                                   {QStringLiteral("id"), id}};
        }
    }
    const bool changed = accepted != SessionRef_;
    const bool invalidChanged = invalid != RefInvalid_;
    SessionRef_ = accepted;
    RefInvalid_ = invalid;
    if (changed) {
        emit sessionRefChanged();
    }
    // The same ref again does not reopen an ended or failed session; reopen()
    // does.
    if (changed || invalidChanged) {
        applySessionRef();
    }
}

QVariant TerminalItem::sessionRef() const {
    return SessionRef_.isValid() ? SessionRef_ : QVariant::fromValue(nullptr);
}

void TerminalItem::reopen() {
    if (Pane_) {
        Pane_->Reopen();
    }
}

void TerminalItem::componentComplete() {
    QQuickItem::componentComplete();
    // The engine and the size are known now; a ref set during creation waited
    // for this.
    applySessionRef();
}

void TerminalItem::applySessionRef() {
    if (!isComponentComplete()) {
        return;
    }
    if (RefInvalid_ || !SessionRef_.isValid()) {
        if (Pane_) {
            Pane_->Show(std::nullopt);
        }
        if (RefInvalid_) {
            setStatus(QStringLiteral("failed"),
                      QStringLiteral("not a SessionRef"));
        } else {
            setStatus(QStringLiteral("none"), {});
        }
        return;
    }
    if (!ensurePane()) {
        return;
    }
    if (!GridKnown_) {
        // Opening now would size the PTY 1x1 and the program would see it;
        // updateGridSize applies the ref once the item has a size.
        setStatus(QStringLiteral("none"), {});
        return;
    }
    const QVariantMap ref = SessionRef_.toMap();
    Pane_->Show(SessionKey{ref.value(QStringLiteral("agent")).toString(),
                           ref.value(QStringLiteral("id")).toString()});
}

bool TerminalItem::ensurePane() {
    if (Pane_) {
        return true;
    }
    QQmlEngine* const engine = qmlEngine(this);
    const auto* const singleton =
        engine != nullptr
            ? engine->singletonInstance<RoostSingleton*>("Roost.Api", "Roost")
            : nullptr;
    api::Api* const windowApi =
        singleton != nullptr ? singleton->api() : nullptr;
    if (windowApi == nullptr) {
        setStatus(QStringLiteral("failed"),
                  QStringLiteral("no roostd API in this window"));
        return false;
    }
    auto* const pane = new SessionPane(windowApi->Terminals(), this);
    Pane_ = pane;
    connect(this, &TerminalItem::input, pane, &SessionPane::SendInput);
    connect(this, &TerminalItem::gridSizeChanged, pane, [this, pane] {
        pane->SetGrid(static_cast<quint16>(Columns_),
                      static_cast<quint16>(Rows_));
    });
    connect(pane, &SessionPane::SessionChanged, this, [this, pane] {
        qCInfo(
            TerminalCategory, "SessionChanged: pane shows %s, session %s",
            qPrintable(pane->Key() ? pane->Key()->Agent + u'/' + pane->Key()->Id
                                   : QStringLiteral("nothing")),
            pane->Session() ? "present" : "none");
        setSession(pane->Session());
    });
    connect(pane, &SessionPane::Updated, this, [this] { update(); });
    connect(pane, &SessionPane::StatusChanged, this, [this, pane] {
        // An invalid ref owns the status ("failed"): the pane's "none" from
        // closing its session must not show in between.
        if (RefInvalid_) {
            return;
        }
        setStatus(statusName(pane->Status()), pane->Message());
    });
    pane->SetGrid(static_cast<quint16>(Columns_), static_cast<quint16>(Rows_));
    return true;
}

void TerminalItem::setStatus(const QString& status, const QString& message) {
    if (status == Status_ && message == StatusMessage_) {
        return;
    }
    qCInfo(TerminalCategory, "status %s -> %s, message \"%s\"",
           qPrintable(Status_), qPrintable(status), qPrintable(message));
    Status_ = status;
    StatusMessage_ = message;
    emit statusChanged();
}

QFont TerminalItem::font() const { return Renderer_.Font(); }

void TerminalItem::setFont(const QFont& font) {
    if (font == Renderer_.Font()) {
        return;
    }
    const auto before = Renderer_.Metrics();
    Renderer_.SetFont(font);
    emit fontChanged();
    if (Renderer_.Metrics() != before) {
        emit cellSizeChanged();
        updateGridSize();
    }
    update();
}

qreal TerminalItem::cellWidth() const { return Renderer_.Metrics().Width; }

qreal TerminalItem::cellHeight() const { return Renderer_.Metrics().Height; }

int TerminalItem::columns() const { return Columns_; }

int TerminalItem::rows() const { return Rows_; }

void TerminalItem::updateGridSize() {
    const auto cell = Renderer_.Metrics();
    // A cell cannot be empty; the guard keeps a broken font from dividing by
    // zero.
    const auto fit = [](qreal size, int cellSize) {
        const auto cells = cellSize > 0 ? std::floor(size / cellSize) : 1.0;
        return static_cast<int>(
            std::clamp(cells, 1.0, double{VtTerminal::MAX_SIZE}));
    };
    const int columns = fit(width(), cell.Width);
    const int rows = fit(height(), cell.Height);
    if (columns != Columns_ || rows != Rows_) {
        Columns_ = columns;
        Rows_ = rows;
        emit gridSizeChanged();
    }
    if (!GridKnown_ && width() > 0 && height() > 0) {
        GridKnown_ = true;
        applySessionRef();
    }
}

void TerminalItem::geometryChange(const QRectF& newGeometry,
                                  const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    updateGridSize();
}

void TerminalItem::keyPressEvent(QKeyEvent* event) {
    handleKey(event,
              event->isAutoRepeat() ? EKeyAction::Repeat : EKeyAction::Press);
}

void TerminalItem::keyReleaseEvent(QKeyEvent* event) {
    handleKey(event, EKeyAction::Release);
}

void TerminalItem::handleKey(QKeyEvent* event, EKeyAction action) {
    // Not ours: let the event go on, to shortcuts and to focus handling.
    event->ignore();
    if (!Session_) {
        return;
    }
    const auto nativeKey = NativeKeyCode(*event, KeyOptions_.NativeKeys);
    if (IsCopyShortcut(event->key(), event->modifiers(), nativeKey,
                       KeyOptions_)) {
        // Taken even when there is nothing to copy, and never sent on: the
        // key release too, which the kitty protocol could report.
        if (action != EKeyAction::Release) {
            copy();
        }
        event->accept();
        return;
    }
    if (!KeyEncoder_) {
        return;
    }
    const auto key = MapQtKey(event->key(), event->modifiers(), event->text(),
                              nativeKey, action, KeyOptions_);
    if (!key) {
        return;
    }
    {
        // Only the sync needs the terminal; the encoder works on its own copy
        // of the modes after it.
        const auto lock = Session_->Lock();
        if (const auto synced = Session_->SyncKeyEncoder(lock, *KeyEncoder_);
            !synced) {
            qWarning("TerminalItem: %s", qPrintable(synced.error()));
            return;
        }
    }
    const auto bytes = KeyEncoder_->Encode(*key);
    if (!bytes) {
        qWarning("TerminalItem: %s", qPrintable(bytes.error()));
        return;
    }
    if (bytes->isEmpty()) {
        return;
    }
    event->accept();
    // The program is about to see what was typed: show where it is typed,
    // and the selected text is no longer what is there (Ghostty's
    // selection-clear-on-typing).
    if (HasSelection_) {
        runSelection([this](const auto& lock) {
            return Session_->ClearSelection(lock);
        });
    }
    scrollToBottom();
    emit input(*bytes);
}

void TerminalItem::scrollToBottom() {
    if (!Session_) {
        return;
    }
    {
        const auto lock = Session_->Lock();
        const auto atBottom = Session_->ViewportAtBottom(lock);
        if (!atBottom) {
            qWarning("TerminalItem: %s", qPrintable(atBottom.error()));
            return;
        }
        if (*atBottom) {
            return;
        }
        if (const auto scrolled = Session_->ScrollViewportToBottom(lock);
            !scrolled) {
            qWarning("TerminalItem: %s", qPrintable(scrolled.error()));
            return;
        }
    }
    update();
}

void TerminalItem::paste(const QString& text) {
    if (!Session_ || text.isEmpty()) {
        return;
    }
    bool bracketed = false;
    {
        // Only the mode needs the terminal.
        const auto lock = Session_->Lock();
        bracketed = Session_->BracketedPaste(lock);
    }
    const auto bytes = VtTerminal::EncodePaste(text, bracketed);
    if (!bytes) {
        qWarning("TerminalItem: %s", qPrintable(bytes.error()));
        return;
    }
    if (!bytes->isEmpty()) {
        scrollToBottom();
        emit input(*bytes);
    }
}

MouseGeometry TerminalItem::mouseGeometry() const {
    // The grid is what the program sees; the pixels the cells do not fill
    // are outside it.
    const auto cell = Renderer_.Metrics();
    return {.ScreenWidth = static_cast<quint32>(Columns_ * cell.Width),
            .ScreenHeight = static_cast<quint32>(Rows_ * cell.Height),
            .CellWidth = static_cast<quint32>(cell.Width),
            .CellHeight = static_cast<quint32>(cell.Height)};
}

QByteArray TerminalItem::encodeMouse(const QList<MouseEvent>& events) {
    if (!Session_ || !MouseEncoder_ || events.isEmpty()) {
        return {};
    }
    const auto geometry = mouseGeometry();
    {
        // Only the sync needs the terminal, as for keys.
        const auto lock = Session_->Lock();
        if (const auto synced =
                Session_->SyncMouseEncoder(lock, *MouseEncoder_, geometry);
            !synced) {
            qWarning("TerminalItem: %s", qPrintable(synced.error()));
            return {};
        }
    }
    QByteArray bytes;
    for (const auto& event : events) {
        const auto encoded = MouseEncoder_->Encode(event);
        if (!encoded) {
            qWarning("TerminalItem: %s", qPrintable(encoded.error()));
            return {};
        }
        bytes += *encoded;
    }
    return bytes;
}

bool TerminalItem::reportMouse(EMouseAction action, EMouseButton button,
                               const QPointF& position,
                               Qt::MouseButtons buttons,
                               Qt::KeyboardModifiers modifiers) {
    MouseEvent event;
    event.Action = action;
    event.Button = button;
    event.Mods = MapQtModifiers(modifiers, KeyOptions_);
    event.X = static_cast<float>(position.x());
    event.Y = static_cast<float>(position.y());
    event.AnyButtonPressed =
        buttons != Qt::NoButton || action == EMouseAction::Press;
    const auto bytes = encodeMouse({event});
    if (bytes.isEmpty()) {
        return false;
    }
    emit input(bytes);
    return true;
}

bool TerminalItem::mouseTracked() const {
    if (!Session_) {
        return false;
    }
    const auto lock = Session_->Lock();
    const auto modes = Session_->ReadWheelModes(lock);
    if (!modes) {
        qWarning("TerminalItem: %s", qPrintable(modes.error()));
        return false;
    }
    return modes->MouseTracking;
}

void TerminalItem::mousePressEvent(QMouseEvent* event) {
    forceActiveFocus(Qt::MouseFocusReason);
    LastPointer_ = event->position();
    // Taking the press is what gets us the moves and the release of a drag;
    // a press nobody asked for stays with whoever is under us.
    if (!Session_) {
        event->ignore();
        return;
    }
    // A program that tracks the mouse owns it, but Shift takes it back for
    // selecting, and the press is then not reported (Ghostty's
    // mouse-shift-capture = false).
    if (mouseTracked() && !event->modifiers().testFlag(Qt::ShiftModifier)) {
        // What the program does with the mouse is its own: no selection
        // stays, as in Ghostty.
        endDrag();
        runSelection([this](const auto& lock) {
            if (const auto reset = Session_->ResetSelectionGesture(lock);
                !reset) {
                return reset;
            }
            return Session_->ClearSelection(lock);
        });
        event->setAccepted(reportMouse(
            EMouseAction::Press, MapQtMouseButton(event->button()),
            event->position(), event->buttons(), event->modifiers()));
        return;
    }
    if (event->button() == Qt::LeftButton) {
        startSelection(event);
    } else {
        event->ignore();
    }
}

void TerminalItem::mouseReleaseEvent(QMouseEvent* event) {
    LastPointer_ = event->position();
    // A press that began a selection ends it, and is not reported, as it
    // never was.
    if (Selecting_ && event->button() == Qt::LeftButton) {
        finishSelection(event);
        return;
    }
    event->setAccepted(
        reportMouse(EMouseAction::Release, MapQtMouseButton(event->button()),
                    event->position(), event->buttons(), event->modifiers()));
}

void TerminalItem::mouseMoveEvent(QMouseEvent* event) {
    LastPointer_ = event->position();
    if (Selecting_ && event->buttons().testFlag(Qt::LeftButton)) {
        dragSelection(event);
        return;
    }
    event->setAccepted(
        reportMouse(EMouseAction::Motion, heldButton(event->buttons()),
                    event->position(), event->buttons(), event->modifiers()));
}

void TerminalItem::mouseUngrabEvent() { endDrag(); }

void TerminalItem::endDrag() {
    Selecting_ = false;
    AutoscrollTimer_.stop();
}

void TerminalItem::startSelection(QMouseEvent* event) {
    const auto geometry = mouseGeometry();
    const VtTerminal::SelectionPointer pointer{.X = event->position().x(),
                                               .Y = event->position().y()};
    // The repeat interval is the platform's double click time.
    using namespace std::chrono;
    const VtTerminal::SelectionClick click{
        .TimeNs = static_cast<quint64>(
            duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
                .count()),
        .RepeatIntervalNs = static_cast<quint64>(
            duration_cast<nanoseconds>(
                milliseconds(
                    QGuiApplication::styleHints()->mouseDoubleClickInterval()))
                .count())};
    Selecting_ = true;
    runSelection([&](const auto& lock) {
        return Session_->SelectionPress(lock, pointer, geometry, click);
    });
    event->accept();
}

void TerminalItem::dragSelection(QMouseEvent* event) {
    const auto geometry = mouseGeometry();
    const VtTerminal::SelectionPointer pointer{.X = event->position().x(),
                                               .Y = event->position().y()};
    runSelection([&](const auto& lock) {
        return Session_->SelectionDrag(lock, pointer, geometry);
    });
    event->accept();
}

void TerminalItem::finishSelection(QMouseEvent* event) {
    const auto geometry = mouseGeometry();
    const VtTerminal::SelectionPointer pointer{.X = event->position().x(),
                                               .Y = event->position().y()};
    Selecting_ = false;
    QString selected;
    runSelection([&](const auto& lock) -> std::expected<void, QString> {
        if (const auto released =
                Session_->SelectionRelease(lock, pointer, geometry);
            !released) {
            return released;
        }
#ifdef Q_OS_LINUX
        // Ghostty's copy-on-select defaults to primary on Linux: the finished
        // selection goes to the selection clipboard, not the system one. On
        // macOS the default is none.
        if (QGuiApplication::clipboard()->supportsSelection()) {
            auto text = Session_->SelectedText(lock);
            if (!text) {
                return std::unexpected(text.error());
            }
            selected = std::move(*text);
        }
#endif
        return {};
    });
    if (!selected.isEmpty()) {
        QGuiApplication::clipboard()->setText(selected, QClipboard::Selection);
    }
    event->accept();
}

void TerminalItem::runSelection(const SelectionStep& step) {
    if (!Session_) {
        return;
    }
    bool selected = false;
    auto autoscroll = VtTerminal::ESelectionAutoscroll::None;
    {
        const auto lock = Session_->Lock();
        if (const auto done = step(lock); !done) {
            qWarning("TerminalItem: %s", qPrintable(done.error()));
        }
        selected = Session_->HasSelection(lock).value_or(false);
        autoscroll = Session_->SelectionAutoscroll(lock).value_or(
            VtTerminal::ESelectionAutoscroll::None);
    }
    const bool scrolling =
        Selecting_ && autoscroll != VtTerminal::ESelectionAutoscroll::None;
    if (scrolling && !AutoscrollTimer_.isActive()) {
        AutoscrollTimer_.start();
    } else if (!scrolling) {
        AutoscrollTimer_.stop();
    }
    if (selected != HasSelection_) {
        HasSelection_ = selected;
        emit hasSelectionChanged();
    }
    update();
}

void TerminalItem::autoscrollTick() {
    const auto geometry = mouseGeometry();
    const VtTerminal::SelectionPointer pointer{.X = LastPointer_.x(),
                                               .Y = LastPointer_.y()};
    runSelection([&](const auto& lock) {
        return Session_->SelectionAutoscrollTick(lock, pointer, geometry);
    });
}

void TerminalItem::syncHasSelection() {
    bool selected = false;
    if (Session_) {
        const auto lock = Session_->Lock();
        selected = Session_->HasSelection(lock).value_or(false);
    }
    if (selected != HasSelection_) {
        HasSelection_ = selected;
        emit hasSelectionChanged();
    }
}

bool TerminalItem::hasSelection() const { return HasSelection_; }

void TerminalItem::copy() {
    if (!Session_) {
        return;
    }
    std::expected<QString, QString> text;
    {
        const auto lock = Session_->Lock();
        text = Session_->SelectedText(lock);
    }
    if (!text) {
        qWarning("TerminalItem: %s", qPrintable(text.error()));
        return;
    }
    if (!text->isEmpty()) {
        QGuiApplication::clipboard()->setText(*text);
    }
}

void TerminalItem::hoverMoveEvent(QHoverEvent* event) {
    reportMouse(EMouseAction::Motion, EMouseButton::None, event->position(),
                Qt::NoButton, event->modifiers());
}

void TerminalItem::wheelEvent(QWheelEvent* event) {
    event->ignore();
    if (!Session_ || !MouseEncoder_) {
        return;
    }
    const auto cell = Renderer_.Metrics();
    // A trackpad says pixels, a wheel says notches; both become pixels, and
    // rows once they add up to one. Like Ghostty, the remainder survives the
    // start of a gesture. A notch sideways is one report, not three: Ghostty
    // sends the rounded notch count as it is.
    qreal dx = 0;
    qreal dy = 0;
    if (!event->pixelDelta().isNull()) {
        dx = event->pixelDelta().x();
        dy = event->pixelDelta().y();
    } else {
        dx = event->angleDelta().x() / UNITS_PER_NOTCH * cell.Width;
        dy = event->angleDelta().y() / UNITS_PER_NOTCH * ROWS_PER_NOTCH *
             cell.Height;
    }
    // Positive: up, and Qt's positive x, which Ghostty reports as button 6.
    const int up = takeWhole(PendingWheelY_, dy, cell.Height);
    const int across = takeWhole(PendingWheelX_, dx, cell.Width);
    event->accept();
    if (up == 0 && across == 0) {
        return;
    }

    const auto geometry = mouseGeometry();
    VtTerminal::WheelModes modes;
    bool scrolled = false;
    {
        const auto lock = Session_->Lock();
        const auto read = Session_->ReadWheelModes(lock);
        if (!read) {
            qWarning("TerminalItem: %s", qPrintable(read.error()));
            return;
        }
        modes = *read;
        const bool arrows = !modes.MouseTracking && modes.AlternateScreen &&
                            modes.AlternateScroll;
        std::expected<void, QString> synced;
        if (modes.MouseTracking) {
            // The wheel goes to the program: the selection (made with Shift)
            // goes, as in Ghostty.
            synced = Session_->ClearSelection(lock);
            if (synced) {
                synced =
                    Session_->SyncMouseEncoder(lock, *MouseEncoder_, geometry);
            }
        } else if (arrows) {
            // Fixed bytes, nothing to sync; the arrows are typing.
            if (up != 0) {
                synced = Session_->ClearSelection(lock);
            }
        } else if (up != 0) {
            // Nothing is tracking, and the alternate screen does not turn the
            // wheel into keys: the user looks through the scrollback. Only
            // the vertical wheel does; the viewport has no sideways.
            synced = Session_->ScrollViewport(lock, -static_cast<qint64>(up));
            scrolled = synced.has_value();
        }
        if (!synced) {
            qWarning("TerminalItem: %s", qPrintable(synced.error()));
            return;
        }
    }
    // Whatever the wheel did to the selection, QML and the frame hear of it.
    syncHasSelection();
    update();
    if (scrolled) {
        return;
    }

    QByteArray bytes;
    const auto append =
        [&bytes](const std::expected<QByteArray, QString>& piece) {
            if (!piece) {
                qWarning("TerminalItem: %s", qPrintable(piece.error()));
                return false;
            }
            bytes += *piece;
            return true;
        };
    if (modes.MouseTracking) {
        MouseEvent report;
        report.Action = EMouseAction::Press;
        report.Mods = MapQtModifiers(event->modifiers(), KeyOptions_);
        report.X = static_cast<float>(event->position().x());
        report.Y = static_cast<float>(event->position().y());
        // A wheel is a press, so one outside the grid is reported in the
        // motion modes only while a button is held.
        report.AnyButtonPressed = event->buttons() != Qt::NoButton;
        const auto send = [&](EMouseButton button, int count) {
            report.Button = button;
            for (int i = 0; i < count; ++i) {
                if (!append(MouseEncoder_->Encode(report))) {
                    return;
                }
            }
        };
        send(up > 0 ? EMouseButton::WheelUp : EMouseButton::WheelDown,
             std::abs(up));
        send(across > 0 ? EMouseButton::WheelLeft : EMouseButton::WheelRight,
             std::abs(across));
    } else if (modes.AlternateScreen && modes.AlternateScroll && up != 0) {
        // Ghostty's fixed bytes, never the kitty form of the key: the program
        // asked for the wheel as arrows, not for key events.
        const char* key = nullptr;
        if (modes.CursorKeysApplication) {
            key = up > 0 ? "\x1bOA" : "\x1bOB";
        } else {
            key = up > 0 ? "\x1b[A" : "\x1b[B";
        }
        for (int i = 0; i < std::abs(up); ++i) {
            bytes += key;
        }
    }
    if (!bytes.isEmpty()) {
        emit input(bytes);
    }
}

int TerminalItem::rowsRebuilt() const { return Renderer_.RowsRebuilt(); }

QSGNode* TerminalItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    if (!Session_ || window() == nullptr) {
        return nullptr;
    }
    if (PaintLogPending_) {
        PaintLogPending_ = false;
        qCDebug(TerminalCategory, "first paint of %s",
                qPrintable(LoggedSession_));
    }
    auto* node = Renderer_.Update(*window(), *Session_, oldNode);
    // The terminal drops a selection when its text is erased; the GUI thread
    // finds out from here.
    if (Session_->SelectionAtLastRender() != HasSelection_) {
        QMetaObject::invokeMethod(this, &TerminalItem::syncHasSelection,
                                  Qt::QueuedConnection);
    }
    return node;
}

}  // namespace roost
