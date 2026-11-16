#pragma once

#include "terminal/renderer.hpp"
#include "terminal/terminal_session.hpp"

#include <QtQml/qqmlregistration.h>
#include <QFont>
#include <QQuickItem>

#include <memory>

namespace roost {

// Shows one TerminalSession. It holds no logic of its own: the drawing is the
// Renderer's (design.md §3, ADR 011). Whoever owns the session calls update()
// when it has new output.
class TerminalItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY fontChanged)
    Q_PROPERTY(qreal cellWidth READ cellWidth NOTIFY cellSizeChanged)
    Q_PROPERTY(qreal cellHeight READ cellHeight NOTIFY cellSizeChanged)

public:
    explicit TerminalItem(QQuickItem* parent = nullptr);

    // From C++ only. The item shares the session; null shows nothing.
    void setSession(std::shared_ptr<TerminalSession> session);

    QFont font() const;
    void setFont(const QFont& font);
    qreal cellWidth() const;
    qreal cellHeight() const;

    // How many rows the last frame rebuilt; for tests and the benchmark.
    int rowsRebuilt() const;

signals:
    void fontChanged();
    void cellSizeChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

private:
    // Both are read and written by the render thread inside updatePaintNode,
    // which runs while the GUI thread is blocked in the scene graph's sync;
    // elsewhere only the GUI thread touches them.
    std::shared_ptr<TerminalSession> Session_;
    Renderer Renderer_;
};

}  // namespace roost
