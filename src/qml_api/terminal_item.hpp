#pragma once

#include "core/vt_key_encoder.hpp"
#include "core/vt_mouse_encoder.hpp"
#include "terminal/qt_key_mapping.hpp"
#include "terminal/renderer.hpp"
#include "terminal/session_pane.hpp"
#include "terminal/terminal_session.hpp"

#include <QtQml/qqmlregistration.h>
#include <QFont>
#include <QList>
#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QTimer>
#include <QVariant>

#include <expected>
#include <functional>
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
// alternate scroll, mode 1007), as Ghostty does.
// Typing and pasting scroll the viewport back to the bottom.
//
// Selecting is Ghostty's too. A left press and drag select by cell, a double
// click a word, a triple click a line, when the program does not track the
// mouse, or when Shift is held while it does (Shift is then not reported). A
// click without a drag clears the selection. A drag past the top or bottom
// edge scrolls the viewport on a 15 ms timer, a row a tick. The selection is
// the terminal's, so it follows the text through output and scrollback.
// Command+C (macOS) or Ctrl+Shift+C copies it as plain text, with wrapped
// lines joined, and sends nothing to the program; with no selection they do
// nothing. A key that sends bytes clears the selection, and so does a wheel
// that goes to the program (reports while it tracks the mouse, arrow keys on
// the alternate screen); scrolling the scrollback keeps it. A drag that loses
// the mouse grab ends where it is. On Linux, as in
// Ghostty, a finished selection also goes to the selection clipboard
// (copy-on-select = primary); on macOS it does not. A click takes the focus.
//
// The `session` property shows a session of roostd in the item: a SessionRef,
// {agent, id}, or null. The item opens the channel itself (SessionPane), at its
// own grid, through the window's API (the singleton Roost), and follows the
// size, the input and the output. It does not open before the item has a size
// (status "none" until then), so the program never sees a 1x1 terminal. The
// same ref again does nothing; reopen() is the way to open an ended session.
// `status` tells how it stands. "none" is no session; "opening" is the open in
// flight, with the screen cleared; "open"; "ended" is a session that ended by
// itself, its last screen kept, and "failed" is an open that was refused or a
// `session` that is no SessionRef, each with `statusMessage` for the reason;
// "disconnected" is a lost connection, the last screen kept, and the item opens
// the same session again when the window is reconnected. reopen() opens an
// ended, failed or disconnected session anew. Without the API of a window (no
// singleton, or no Api set in it) the status is "failed".
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
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY hasSelectionChanged)
    // A SessionRef, {agent, id}, or null. Reads back as the ref that was
    // accepted (null for an invalid one).
    Q_PROPERTY(QVariant session READ sessionRef WRITE setSessionRef NOTIFY
                   sessionRefChanged)
    // "none", "opening", "open", "ended", "failed" or "disconnected".
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusChanged)

public:
    explicit TerminalItem(QQuickItem* parent = nullptr);

    // From C++ only. The item shares the session; null shows nothing. For a
    // session that C++ owns (SingleSession). Do not mix it with the `session`
    // property, whose pane sets and clears the shared session on its own.
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

    QVariant sessionRef() const;
    void setSessionRef(const QVariant& ref);
    QString status() const { return Status_; }
    QString statusMessage() const { return StatusMessage_; }
    // Opens the session again if it is ended, failed or disconnected.
    Q_INVOKABLE void reopen();

    bool hasSelection() const;
    // Puts the selected text on the system clipboard; nothing is copied, and
    // the clipboard stays as it was, without a selection.
    Q_INVOKABLE void copy();

    // How many rows the last frame rebuilt; for tests and the benchmark.
    int rowsRebuilt() const;

signals:
    void fontChanged();
    void cellSizeChanged();
    void gridSizeChanged();
    void hasSelectionChanged();
    void sessionRefChanged();
    // status or statusMessage changed.
    void statusChanged();
    // Bytes to write to the program.
    void input(const QByteArray& bytes);

protected:
    void componentComplete() override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    // The grab is gone (window, popup or scene took it) with no release: the
    // drag is over, and what it selected stays.
    void mouseUngrabEvent() override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void geometryChange(const QRectF& newGeometry,
                        const QRectF& oldGeometry) override;
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

private:
    void handleKey(QKeyEvent* event, EKeyAction action);
    // Whether the program tracks the mouse now.
    bool mouseTracked() const;
    using SelectionStep = std::function<std::expected<void, QString>(
        const TerminalSession::Locked&)>;
    // Runs a step on the selection under one lock, then learns what it did:
    // whether there is a selection, whether the drag wants the viewport to
    // scroll (the timer follows), and asks for a frame.
    void runSelection(const SelectionStep& step);
    void autoscrollTick();
    // The drag stops: no more selection steps and no autoscroll. The
    // selection made so far stays.
    void endDrag();
    // Reads whether the terminal has a selection and tells QML if it changed.
    void syncHasSelection();
    // The left button press, its drag and its release, as selecting.
    void startSelection(QMouseEvent* event);
    void finishSelection(QMouseEvent* event);
    void dragSelection(QMouseEvent* event);
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
    // Shows SessionRef_ in the pane, making the pane on the first session.
    void applySessionRef();
    // Makes the pane and wires it to the item and the Api; false (and the
    // status "failed") if the window has no API.
    bool ensurePane();
    void setStatus(const QString& status, const QString& message);
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
    // GUI thread only (the render thread only reads HasSelection_ while the
    // GUI thread is blocked).
    // A left press went to the selection and the button is still down.
    bool Selecting_ = false;
    bool HasSelection_ = false;
    // Where the pointer was last, for the autoscroll ticks.
    QPointF LastPointer_;
    QTimer AutoscrollTimer_;
    // The accepted ref: a map with agent and id, or invalid.
    QVariant SessionRef_;
    // The last value was not a SessionRef.
    bool RefInvalid_ = false;
    // The item has had a real size; a session is not opened before, or the
    // PTY would start at 1x1.
    bool GridKnown_ = false;
    // The pane is a child of the item; made when the first session is shown.
    QPointer<SessionPane> Pane_;
    QString Status_ = QStringLiteral("none");
    QString StatusMessage_;
};

}  // namespace roost
