#include "qml_api/terminal_item.hpp"

#include <QQuickWindow>

namespace roost {

TerminalItem::TerminalItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
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
    }
    update();
}

qreal TerminalItem::cellWidth() const { return Renderer_.Metrics().Width; }

qreal TerminalItem::cellHeight() const { return Renderer_.Metrics().Height; }

int TerminalItem::rowsRebuilt() const { return Renderer_.RowsRebuilt(); }

QSGNode* TerminalItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    if (!Session_ || window() == nullptr) {
        return nullptr;
    }
    return Renderer_.Update(*window(), *Session_, oldNode);
}

}  // namespace roost
