#include "qml_api/terminal_item.hpp"

#include "core/vt_terminal.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QWheelEvent>
#include <QtLogging>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace roost {

namespace {

// Rows a notch of a wheel scrolls; Ghostty's default for a wheel that is not
// a trackpad. Qt reports a notch as 120 units.
constexpr qreal ROWS_PER_NOTCH = 3;
constexpr qreal UNITS_PER_NOTCH = 120;

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
}

void TerminalItem::setSession(std::shared_ptr<TerminalSession> session) {
    Session_ = std::move(session);
    // The old session's rows are still in the scene; a clean session on the
    // same grid would leave them there.
    Renderer_.Invalidate();
    update();
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
    if (columns == Columns_ && rows == Rows_) {
        return;
    }
    Columns_ = columns;
    Rows_ = rows;
    emit gridSizeChanged();
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
    if (!Session_ || !KeyEncoder_) {
        return;
    }
    const auto key = MapQtKey(event->key(), event->modifiers(), event->text(),
                              action, KeyOptions_);
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
    // The program is about to see what was typed: show where it is typed.
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

void TerminalItem::mousePressEvent(QMouseEvent* event) {
    // Taking the press is what gets us the moves and the release of a drag;
    // a press nobody asked for stays with whoever is under us.
    event->setAccepted(
        reportMouse(EMouseAction::Press, MapQtMouseButton(event->button()),
                    event->position(), event->buttons(), event->modifiers()));
}

void TerminalItem::mouseReleaseEvent(QMouseEvent* event) {
    event->setAccepted(
        reportMouse(EMouseAction::Release, MapQtMouseButton(event->button()),
                    event->position(), event->buttons(), event->modifiers()));
}

void TerminalItem::mouseMoveEvent(QMouseEvent* event) {
    event->setAccepted(
        reportMouse(EMouseAction::Motion, heldButton(event->buttons()),
                    event->position(), event->buttons(), event->modifiers()));
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
            synced = Session_->SyncMouseEncoder(lock, *MouseEncoder_, geometry);
        } else if (arrows) {
            // Fixed bytes, nothing to sync.
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
    if (scrolled) {
        update();
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
    return Renderer_.Update(*window(), *Session_, oldNode);
}

}  // namespace roost
