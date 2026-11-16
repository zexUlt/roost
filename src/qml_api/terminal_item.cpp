#include "qml_api/terminal_item.hpp"

#include "core/vt_terminal.hpp"

#include <QKeyEvent>
#include <QQuickWindow>
#include <QtLogging>

#include <algorithm>
#include <cmath>

namespace roost {

TerminalItem::TerminalItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
    setActiveFocusOnTab(true);
    if (auto encoder = VtKeyEncoder::Create()) {
        KeyEncoder_.emplace(std::move(*encoder));
        KeyEncoder_->SetMacosOptionAsAlt(KeyOptions_.OptionAsAlt);
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
    emit input(*bytes);
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
        emit input(*bytes);
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
