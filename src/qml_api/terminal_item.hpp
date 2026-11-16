#pragma once

#include "core/vt_key_encoder.hpp"
#include "core/vt_mouse_encoder.hpp"
#include "terminal/qt_key_mapping.hpp"
#include "terminal/renderer.hpp"
#include "terminal/terminal_session.hpp"

#include <QtQml/qqmlregistration.h>
#include <QFont>
#include <QList>
#include <QQuickItem>

#include <memory>
#include <optional>

namespace roost {

// Shows one TerminalSession. It holds no logic of its own: the drawing is the
// Renderer's (design.md §3, ADR 011). Whoever owns the session calls update()
// when it has new output.
//
// Input leaves through `input` as the bytes for the program, and goes nowhere
// else: not into the local terminal, whose copy of the screen is changed only
// by the output the PTY echoes back (design.md §3). The owner connects `input`
// to the session's channel.
//
// The mouse goes out the same way: when the program tracks it (DECSET 9,
// 1000, 1002, 1003), clicks, drags, motion and the wheel become the reports
// it asked for. When it does not, the wheel scrolls the viewport through the
// scrollback (primary screen) or types cursor keys (alternate screen with
// alternate scroll, mode 1007), as Ghostty does; a click does nothing yet.
// Typing and pasting scroll the viewport back to the bottom.
//
// The item does not resize the session either. It says how many columns and
// rows fit (`gridSizeChanged`), and the owner sends session.resize, which
// resizes the local terminal as well.
class TerminalItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY fontChanged)
    Q_PROPERTY(qreal cellWidth READ cellWidth NOTIFY cellSizeChanged)
    Q_PROPERTY(qreal cellHeight READ cellHeight NOTIFY cellSizeChanged)
    // Whole cells that fit in the item: at least 1, at most
    // VtTerminal::MAX_SIZE, the bound the terminal accepts.
    Q_PROPERTY(int columns READ columns NOTIFY gridSizeChanged)
    Q_PROPERTY(int rows READ rows NOTIFY gridSizeChanged)

public:
    explicit TerminalItem(QQuickItem* parent = nullptr);

    // From C++ only. The item shares the session; null shows nothing.
    void setSession(std::shared_ptr<TerminalSession> session);

    QFont font() const;
    void setFont(const QFont& font);
    qreal cellWidth() const;
    qreal cellHeight() const;
    int columns() const;
    int rows() const;

    // Text the user pastes, as the bytes for the program: wrapped in
    // bracketed paste markers if the terminal asked for them. Nothing is
    // emitted for an empty text or without a session.
    Q_INVOKABLE void paste(const QString& text);

    // How many rows the last frame rebuilt; for tests and the benchmark.
    int rowsRebuilt() const;

signals:
    void fontChanged();
    void cellSizeChanged();
    void gridSizeChanged();
    // Bytes to write to the program.
    void input(const QByteArray& bytes);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void geometryChange(const QRectF& newGeometry,
                        const QRectF& oldGeometry) override;
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

private:
    void handleKey(QKeyEvent* event, EKeyAction action);
    MouseGeometry mouseGeometry() const;
    // The reports for these events, from one sync of the encoder under a
    // short lock; empty if the terminal does not track the mouse.
    QByteArray encodeMouse(const QList<MouseEvent>& events);
    // One press, release or motion; true if it was reported.
    bool reportMouse(EMouseAction action, EMouseButton button,
                     const QPointF& position, Qt::MouseButtons buttons,
                     Qt::KeyboardModifiers modifiers);
    // Scrolls the viewport to the bottom if it is not there, and asks for a
    // frame.
    void scrollToBottom();
    // Recomputes the grid from the size and the cell; emits gridSizeChanged
    // if it differs.
    void updateGridSize();

    // Both are read and written by the render thread inside updatePaintNode,
    // which runs while the GUI thread is blocked in the scene graph's sync;
    // elsewhere only the GUI thread touches them.
    std::shared_ptr<TerminalSession> Session_;
    Renderer Renderer_;
    // GUI thread only. Empty if the library could not make one: keys are then
    // ignored.
    std::optional<VtKeyEncoder> KeyEncoder_;
    // The same for the mouse: without one, the mouse is ignored.
    std::optional<VtMouseEncoder> MouseEncoder_;
    // Wheel movement in pixels that has not added up to a whole row yet.
    qreal PendingWheelX_ = 0;
    qreal PendingWheelY_ = 0;
    QtKeyOptions KeyOptions_ = QtKeyOptions::ForThisPlatform();
    int Columns_ = 1;
    int Rows_ = 1;
};

}  // namespace roost
