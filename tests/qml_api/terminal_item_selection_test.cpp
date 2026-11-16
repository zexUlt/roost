// A focused TerminalItem in a real QQuickWindow on the offscreen platform:
// selecting and copying, as Ghostty does it. A left drag selects by cell, a
// double click a word, a triple click a line, and a click without a drag
// clears; while the program tracks the mouse that is Shift's, and a plain
// drag is reported to the program instead. The selection lives in the
// terminal, so it stays on the same text through output and scrolling. The
// selected cells are drawn in the selection colors. Command+C (macOS) or
// Ctrl+Shift+C copies it and sends nothing; typing clears it. The clipboard
// is the offscreen platform's own, in this process: the real one is never
// touched.

#include "qml_api/terminal_item.hpp"
#include "terminal/qt_key_mapping.hpp"
#include "terminal/renderer.hpp"
#include "terminal/terminal_session.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>

#include <memory>

using roost::TerminalItem;
using roost::TerminalSession;

namespace {

constexpr quint16 COLUMNS = 20;
constexpr quint16 ROWS = 8;
constexpr int FRAME_TIMEOUT_MS = 10000;
// How long to wait for something that should happen.
constexpr int HAPPENS_MS = 3000;
// How long to wait for something that should not.
constexpr int NOT_HAPPENS_MS = 300;
constexpr int NOTCH = 120;
// What the clipboard holds when a test says nothing was copied.
const QString KEPT = QStringLiteral("kept");

std::shared_ptr<TerminalSession> NewSession() {
    auto session = TerminalSession::Create(COLUMNS, ROWS);
    if (!session) {
        qFatal("%s", qPrintable(session.error()));
    }
    return std::shared_ptr<TerminalSession>(std::move(*session));
}

void Write(TerminalSession& session, QByteArrayView bytes) {
    const auto lock = session.Lock();
    session.Write(lock, bytes);
}

// "L00\r\nL01\r\n ... ": the last lines are on the screen, the rest in the
// scrollback.
QByteArray Lines(int from, int to) {
    QByteArray text;
    for (int line = from; line <= to; ++line) {
        text +=
            QByteArray("L") + QByteArray::number(line).rightJustified(2, '0');
        if (line != to) {
            text += "\r\n";
        }
    }
    return text;
}

constexpr int LAST_LINE = 29;

bool CommandIsControl() {
    return roost::QtKeyOptions::ForThisPlatform().SwapControlAndMeta;
}

// The platform's copy shortcut, and its interrupt.
Qt::KeyboardModifiers CopyModifiers() {
    return CommandIsControl() ? Qt::ControlModifier
                              : Qt::ControlModifier | Qt::ShiftModifier;
}

Qt::KeyboardModifiers InterruptModifiers() {
    return CommandIsControl() ? Qt::MetaModifier : Qt::ControlModifier;
}

QClipboard* Clipboard() { return QGuiApplication::clipboard(); }

}  // namespace

class TerminalItemSelectionTest : public QObject {
    Q_OBJECT

private:
    void Show(std::shared_ptr<TerminalSession> session) {
        Session_ = std::move(session);
        Item_ = new TerminalItem(Window_.contentItem());
        Item_->setSession(Session_);
        Cell_ = roost::MeasureCell(Item_->font());
        Item_->setSize(QSizeF(COLUMNS * Cell_.Width, ROWS * Cell_.Height));
        Window_.resize(COLUMNS * Cell_.Width, ROWS * Cell_.Height);
        QSignalSpy swapped(&Window_, &QQuickWindow::frameSwapped);
        Window_.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Window_));
        QVERIFY(QTest::qWaitForWindowActive(&Window_));
        QVERIFY(swapped.count() > 0 || swapped.wait(FRAME_TIMEOUT_MS));
        Item_->forceActiveFocus();
        QVERIFY(QTest::qWaitFor([&] { return Item_->hasActiveFocus(); }));
    }

    // Where in a cell, in window coordinates: the left quarter picks the
    // cell as the start of a selection, the right quarter as its end.
    QPoint CellLeft(int column, int row) const {
        return {column * Cell_.Width + Cell_.Width / 4,
                row * Cell_.Height + Cell_.Height / 2};
    }
    QPoint CellRight(int column, int row) const {
        return {column * Cell_.Width + Cell_.Width * 3 / 4,
                row * Cell_.Height + Cell_.Height / 2};
    }
    QPoint CellCenter(int column, int row) const {
        return {column * Cell_.Width + Cell_.Width / 2,
                row * Cell_.Height + Cell_.Height / 2};
    }

    // A drag from the start of one cell to the end of another.
    void Drag(QPoint from, QPoint to, Qt::KeyboardModifiers mods = {}) {
        QTest::mousePress(&Window_, Qt::LeftButton, mods, from);
        QTest::mouseMove(&Window_, to);
        QTest::mouseRelease(&Window_, Qt::LeftButton, mods, to);
    }

    // What copy() puts on the clipboard; null if it put nothing.
    QString Copied() {
        Clipboard()->setText(KEPT);
        Item_->copy();
        const auto text = Clipboard()->text();
        return text == KEPT ? QString() : text;
    }

    // Waits for the sync of a frame the item asked for: a swap could be of a
    // frame synced before, and leave the render state behind.
    bool DrawFrame() {
        QSignalSpy synced(&Window_, &QQuickWindow::afterSynchronizing);
        Item_->update();
        return synced.wait(HAPPENS_MS);
    }

    QString RowText(int row) const {
        QString text;
        for (const auto& cell : Session_->RenderState().Rows()[row].Cells) {
            text += cell.Text.isEmpty() ? QStringLiteral(" ") : cell.Text;
        }
        return text.trimmed();
    }

    bool AtBottom() const {
        const auto lock = Session_->Lock();
        return Session_->ViewportAtBottom(lock).value_or(false);
    }

    void ShowWithScrollback() {
        auto session = NewSession();
        Write(*session, Lines(0, LAST_LINE));
        Show(session);
        QCOMPARE(RowText(0), QStringLiteral("L22"));
    }

    void SendWheel(int delta) {
        QWheelEvent wheel(
            QPointF(5, 5), QPointF(Window_.mapToGlobal(QPoint(5, 5))), {},
            QPoint(0, delta), Qt::NoButton, {}, Qt::NoScrollPhase, false);
        wheel.setAccepted(false);
        QCoreApplication::sendEvent(&Window_, &wheel);
    }

    void SendKey(Qt::Key key, Qt::KeyboardModifiers mods,
                 const QString& text = {}) {
        QKeyEvent press(QEvent::KeyPress, key, mods, text);
        QCoreApplication::sendEvent(&Window_, &press);
        QKeyEvent release(QEvent::KeyRelease, key, mods);
        QCoreApplication::sendEvent(&Window_, &release);
    }

private slots:
    void initTestCase() {
        // Selecting copies on Linux, and a test must not reach the real
        // clipboard of a developer's desktop.
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
    }

    void init() { Window_.setColor(Qt::magenta); }

    void cleanup() {
        Window_.hide();
        delete Item_;
        Item_ = nullptr;
        Session_.reset();
    }

    void DragSelectsTheCellsItCovers() {
        auto session = NewSession();
        Write(*session, "hello world\r\nsecond line\r\nthird");
        Show(session);
        QVERIFY(!Item_->hasSelection());
        QSignalSpy changed(Item_, &TerminalItem::hasSelectionChanged);

        Drag(CellLeft(0, 0), CellRight(4, 0));
        QVERIFY(Item_->hasSelection());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(Copied(), QStringLiteral("hello"));

        // Backwards and over lines: the ends are the cells, in any order.
        Drag(CellRight(2, 1), CellLeft(6, 0));
        QCOMPARE(Copied(), QStringLiteral("world\nsec"));
        Drag(CellLeft(1, 2), CellRight(3, 2));
        QCOMPARE(Copied(), QStringLiteral("hir"));
    }

    void SpacesAtTheEndOfALineAreNotCopied() {
        auto session = NewSession();
        // Spaces the program typed, not blank cells.
        Write(*session, "ab   \r\ncd");
        Show(session);

        Drag(CellLeft(0, 0), CellRight(3, 1));
        QCOMPARE(Copied(), QStringLiteral("ab\ncd"));
    }

    void ALineTheTerminalWrappedCopiesAsOne() {
        auto session = NewSession();
        // Thirty characters on twenty columns: one line on two rows.
        Write(*session, "abcdefghijklmnopqrstuvwxyz0123\r\nnext");
        Show(session);
        QCOMPARE(RowText(0), QStringLiteral("abcdefghijklmnopqrst"));

        Drag(CellLeft(0, 0), CellRight(19, 1));
        QCOMPARE(Copied(), QStringLiteral("abcdefghijklmnopqrstuvwxyz0123"));

        // A real line break stays one.
        Drag(CellLeft(0, 0), CellRight(3, 2));
        QCOMPARE(Copied(),
                 QStringLiteral("abcdefghijklmnopqrstuvwxyz0123\nnext"));
    }

    void DoubleClickSelectsAWord() {
        auto session = NewSession();
        Write(*session, "foo bar-baz qux");
        Show(session);

        QTest::mouseDClick(&Window_, Qt::LeftButton, {}, CellCenter(5, 0));
        QVERIFY(Item_->hasSelection());
        QCOMPARE(Copied(), QStringLiteral("bar-baz"));
    }

    void TripleClickSelectsALine() {
        auto session = NewSession();
        Write(*session, "one\r\nfoo bar baz\r\nthree");
        Show(session);

        QTest::mouseDClick(&Window_, Qt::LeftButton, {}, CellCenter(5, 1));
        QCOMPARE(Copied(), QStringLiteral("bar"));
        QTest::mouseClick(&Window_, Qt::LeftButton, {}, CellCenter(5, 1));
        QCOMPARE(Copied(), QStringLiteral("foo bar baz"));
    }

    void AClickWithoutADragClears() {
        auto session = NewSession();
        Write(*session, "hello world\r\nsecond line");
        Show(session);
        Drag(CellLeft(0, 0), CellRight(4, 0));
        QVERIFY(Item_->hasSelection());
        QSignalSpy changed(Item_, &TerminalItem::hasSelectionChanged);

        QTest::mouseClick(&Window_, Qt::LeftButton, {}, CellCenter(10, 1));
        QVERIFY(!Item_->hasSelection());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(Copied(), QString());
        // Nothing to draw selected either.
        QVERIFY(DrawFrame());
        for (const auto& row : Session_->RenderState().Rows()) {
            QVERIFY(!row.Selection.has_value());
        }
    }

    void AClickTakesTheFocus() {
        Show(NewSession());
        Item_->setFocus(false, Qt::OtherFocusReason);
        QVERIFY(QTest::qWaitFor([&] { return !Item_->hasActiveFocus(); }));

        QTest::mouseClick(&Window_, Qt::LeftButton, {}, CellCenter(3, 3));
        QVERIFY(Item_->hasActiveFocus());
    }

    void ShiftDragSelectsWhileTheProgramTracksTheMouse() {
        auto session = NewSession();
        Write(*session, "hello world\r\nsecond line\x1b[?1002h\x1b[?1006h");
        Show(session);
        QSignalSpy input(Item_, &TerminalItem::input);

        Drag(CellLeft(0, 0), CellRight(4, 0), Qt::ShiftModifier);
        QVERIFY(Item_->hasSelection());
        QCOMPARE(Copied(), QStringLiteral("hello"));
        // Shift is the user's, not the program's: not one report.
        QCOMPARE(input.count(), 0);
    }

    void APlainDragIsReportedWhenTheProgramTracksTheMouse() {
        auto session = NewSession();
        Write(*session, "hello world\r\nsecond line");
        Show(session);
        // A selection made before the program turned tracking on goes
        // when the program takes the mouse.
        Drag(CellLeft(0, 0), CellRight(4, 0));
        QVERIFY(Item_->hasSelection());
        Write(*Session_, "\x1b[?1002h\x1b[?1006h");
        QSignalSpy input(Item_, &TerminalItem::input);

        Drag(CellLeft(0, 1), CellRight(4, 1));
        QCOMPARE(input.count(), 3);
        QCOMPARE(input.at(0).at(0).toByteArray(), QByteArray("\x1b[<0;1;2M"));
        QCOMPARE(input.at(1).at(0).toByteArray(), QByteArray("\x1b[<32;5;2M"));
        QCOMPARE(input.at(2).at(0).toByteArray(), QByteArray("\x1b[<0;5;2m"));
        QVERIFY(!Item_->hasSelection());
        QCOMPARE(Copied(), QString());
    }

    void TheSelectionStaysOnItsTextThroughOutputAndScrolling() {
        ShowWithScrollback();
        // L24 and L25, on rows 2 and 3.
        Drag(CellLeft(0, 2), CellRight(2, 3));
        QCOMPARE(Copied(), QStringLiteral("L24\nL25"));

        // Output pushes the lines up the screen: same text, other rows.
        Write(*Session_, "\r\n" + Lines(30, 32));
        QVERIFY(DrawFrame());
        QVERIFY(Item_->hasSelection());
        QCOMPARE(RowText(0), QStringLiteral("L25"));
        QCOMPARE(Copied(), QStringLiteral("L24\nL25"));

        // And through the scrollback: up and back down.
        for (const int delta : {NOTCH * 2, -NOTCH * 2}) {
            QWheelEvent wheel(
                QPointF(5, 5), QPointF(Window_.mapToGlobal(QPoint(5, 5))), {},
                QPoint(0, delta), Qt::NoButton, {}, Qt::NoScrollPhase, false);
            wheel.setAccepted(false);
            QCoreApplication::sendEvent(&Window_, &wheel);
            QVERIFY(DrawFrame());
            QVERIFY(Item_->hasSelection());
            QCOMPARE(Copied(), QStringLiteral("L24\nL25"));
        }
    }

    void SelectedCellsAreDrawnInTheSelectionColors() {
        auto session = NewSession();
        Write(*session, "hello");
        Show(session);
        const auto fg = Session_->RenderState().DefaultForeground();
        const auto bg = Session_->RenderState().DefaultBackground();
        const auto pixelAt = [&](int column, int row) {
            const auto image = Window_.grabWindow();
            return image
                .pixelColor(static_cast<int>((column + 0.5) * Cell_.Width *
                                             image.devicePixelRatio()),
                            static_cast<int>((row + 0.5) * Cell_.Height *
                                             image.devicePixelRatio()))
                .rgb();
        };
        const auto plain = qRgb(bg.R, bg.G, bg.B);
        const auto inverted = qRgb(fg.R, fg.G, fg.B);
        QVERIFY(plain != inverted);

        // Blank cells of row 5, so that no glyph is in the way: selected,
        // they swap the colors.
        Drag(CellLeft(2, 5), CellRight(6, 5));
        QVERIFY(DrawFrame());

        QCOMPARE(pixelAt(2, 5), inverted);
        QCOMPARE(pixelAt(6, 5), inverted);
        QCOMPARE(pixelAt(1, 5), plain);
        QCOMPARE(pixelAt(7, 5), plain);
        QCOMPARE(pixelAt(4, 4), plain);

        // And back, when the selection goes.
        QTest::mouseClick(&Window_, Qt::LeftButton, {}, CellCenter(10, 2));
        QVERIFY(DrawFrame());
        QCOMPARE(pixelAt(4, 5), plain);

        // A frame that nothing asked for rebuilds nothing.
        QVERIFY(DrawFrame());
        QCOMPARE(Item_->rowsRebuilt(), 0);
    }

    void TheCopyShortcutCopiesAndSendsNothing() {
        auto session = NewSession();
        Write(*session, "hello world");
        Show(session);
        Drag(CellLeft(0, 0), CellRight(4, 0));
        QSignalSpy input(Item_, &TerminalItem::input);

        Clipboard()->setText(KEPT);
        SendKey(
            Qt::Key_C, CopyModifiers(),
            CommandIsControl() ? QStringLiteral("c") : QStringLiteral("\x03"));
        QCOMPARE(Clipboard()->text(), QStringLiteral("hello"));
        QCOMPARE(input.count(), 0);
        // Copying leaves the selection where it is.
        QVERIFY(Item_->hasSelection());
    }

    void TheInterruptKeyStillGoesToTheProgram() {
        auto session = NewSession();
        Write(*session, "hello world");
        Show(session);
        Drag(CellLeft(0, 0), CellRight(4, 0));
        QSignalSpy input(Item_, &TerminalItem::input);

        Clipboard()->setText(KEPT);
        SendKey(Qt::Key_C, InterruptModifiers(), QStringLiteral("\x03"));
        QCOMPARE(input.count(), 1);
        QCOMPARE(input.at(0).at(0).toByteArray(), QByteArray("\x03"));
        QCOMPARE(Clipboard()->text(), KEPT);
    }

    void WithoutASelectionTheCopyShortcutDoesNothing() {
        Show(NewSession());
        Write(*Session_, "hello");
        QSignalSpy input(Item_, &TerminalItem::input);

        Clipboard()->setText(KEPT);
        SendKey(Qt::Key_C, CopyModifiers());
        QCOMPARE(Clipboard()->text(), KEPT);
        QCOMPARE(input.count(), 0);
        Item_->copy();
        QCOMPARE(Clipboard()->text(), KEPT);
    }

    void TypingClearsTheSelection() {
        auto session = NewSession();
        Write(*session, "hello world");
        Show(session);
        Drag(CellLeft(0, 0), CellRight(4, 0));
        QVERIFY(Item_->hasSelection());
        QSignalSpy input(Item_, &TerminalItem::input);

        // A modifier alone sends nothing and leaves the selection.
        SendKey(Qt::Key_Shift, Qt::ShiftModifier);
        QVERIFY(Item_->hasSelection());
        QCOMPARE(input.count(), 0);

        SendKey(Qt::Key_X, {}, QStringLiteral("x"));
        QCOMPARE(input.count(), 1);
        QVERIFY(!Item_->hasSelection());
        QCOMPARE(Copied(), QString());
    }

    void ADragPastTheEdgeScrollsTheViewport() {
        ShowWithScrollback();
        // From L26 (row 4) up and out of the top of the item.
        QTest::mousePress(&Window_, Qt::LeftButton, {}, CellRight(2, 4));
        QTest::mouseMove(&Window_, CellLeft(0, 1));
        QTest::mouseMove(&Window_, QPoint(CellLeft(0, 0).x(), -5));
        QVERIFY(QTest::qWaitFor([&] { return !AtBottom(); }, HAPPENS_MS));
        // It keeps going while the pointer stays out, a row a tick.

        QVERIFY(QTest::qWaitFor(
            [&] { return Copied().startsWith(QStringLiteral("L00")); },
            HAPPENS_MS));
        QTest::mouseRelease(&Window_, Qt::LeftButton, {},
                            QPoint(CellLeft(0, 0).x(), -5));
        QVERIFY(Item_->hasSelection());
        QVERIFY(Copied().endsWith(QStringLiteral("L26")));

        // Let go: it stops.
        QVERIFY(DrawFrame());
        const auto top = RowText(0);
        QTest::qWait(NOT_HAPPENS_MS);
        QVERIFY(DrawFrame());
        QCOMPARE(RowText(0), top);

        // The other way: scrolled up, a drag below the bottom edge comes
        // back down.
        QTest::mousePress(&Window_, Qt::LeftButton, {}, CellLeft(0, 3));
        QTest::mouseMove(&Window_, CellRight(2, 6));
        QTest::mouseMove(&Window_,
                         QPoint(CellRight(2, 7).x(), ROWS * Cell_.Height + 5));
        QVERIFY(QTest::qWaitFor([&] { return AtBottom(); }, HAPPENS_MS));
        QTest::mouseRelease(&Window_, Qt::LeftButton, {},
                            QPoint(10, ROWS * Cell_.Height + 5));
    }

    void ALostGrabEndsTheDragAndKeepsTheSelection() {
        ShowWithScrollback();
        QTest::mousePress(&Window_, Qt::LeftButton, {}, CellRight(2, 4));
        QTest::mouseMove(&Window_, CellLeft(0, 1));
        QTest::mouseMove(&Window_, QPoint(CellLeft(0, 0).x(), -5));
        QVERIFY(QTest::qWaitFor([&] { return !AtBottom(); }, HAPPENS_MS));

        // The scene takes the grab away, with no release.
        Item_->ungrabMouse();
        QVERIFY(DrawFrame());
        const auto top = RowText(0);
        QTest::qWait(NOT_HAPPENS_MS * 2);
        QVERIFY(DrawFrame());
        QCOMPARE(RowText(0), top);
        QVERIFY(Item_->hasSelection());
        QVERIFY(Copied().endsWith(QStringLiteral("L26")));
        QTest::mouseRelease(&Window_, Qt::LeftButton, {},
                            QPoint(CellLeft(0, 0).x(), -5));
    }

    void TheWheelGoingToTheProgramAsReportsClearsTheSelection() {
        auto session = NewSession();
        Write(*session, "hello world\x1b[?1000h\x1b[?1006h");
        Show(session);
        Drag(CellLeft(0, 0), CellRight(4, 0), Qt::ShiftModifier);
        QVERIFY(Item_->hasSelection());
        QSignalSpy input(Item_, &TerminalItem::input);

        SendWheel(NOTCH);
        QVERIFY(input.count() > 0);
        QVERIFY(!Item_->hasSelection());
        QCOMPARE(Copied(), QString());
    }

    void TheWheelGoingToTheProgramAsArrowsClearsTheSelection() {
        auto session = NewSession();
        Write(*session, "\x1b[?1049h\x1b[?1007hhello world");
        Show(session);
        Drag(CellLeft(0, 0), CellRight(4, 0));
        QVERIFY(Item_->hasSelection());
        QSignalSpy input(Item_, &TerminalItem::input);

        SendWheel(NOTCH);
        QCOMPARE(input.count(), 1);
        QVERIFY(!Item_->hasSelection());
        QCOMPARE(Copied(), QString());
    }

    void TheWheelOnTheAlternateScreenWithoutArrowsKeepsTheSelection() {
        auto session = NewSession();
        Write(*session, "\x1b[?1049h\x1b[?1007lhello world");
        Show(session);
        Drag(CellLeft(0, 0), CellRight(4, 0));
        QSignalSpy input(Item_, &TerminalItem::input);

        SendWheel(NOTCH);
        QCOMPARE(input.count(), 0);
        QVERIFY(Item_->hasSelection());
        QCOMPARE(Copied(), QStringLiteral("hello"));
    }

    void ASessionSwapEndsTheDrag() {
        ShowWithScrollback();
        // The new session tracks the mouse: what the old gesture's button
        // does next is its to see, not the old selection's.
        auto next = NewSession();
        Write(*next, "other text\x1b[?1002h\x1b[?1006h");
        QTest::mousePress(&Window_, Qt::LeftButton, {}, CellRight(2, 4));
        QTest::mouseMove(&Window_, CellLeft(0, 1));
        QTest::mouseMove(&Window_, QPoint(CellLeft(0, 0).x(), -5));
        QVERIFY(QTest::qWaitFor([&] { return !AtBottom(); }, HAPPENS_MS));
        QVERIFY(Item_->hasSelection());

        Item_->setSession(next);
        Session_ = next;
        QVERIFY(!Item_->hasSelection());
        QSignalSpy input(Item_, &TerminalItem::input);
        QTest::mouseMove(&Window_, CellRight(8, 0));
        QTest::qWait(NOT_HAPPENS_MS);
        QTest::mouseRelease(&Window_, Qt::LeftButton, {}, CellRight(8, 0));
        QCOMPARE(input.count(), 2);
        QVERIFY(!Item_->hasSelection());
    }

    void FinishingASelectionCopiesOnlyWhereGhosttyDoes() {
        auto session = NewSession();
        Write(*session, "hello world");
        Show(session);
        Clipboard()->setText(KEPT);

        Drag(CellLeft(0, 0), CellRight(4, 0));
#ifdef Q_OS_LINUX
        // copy-on-select = primary: the selection clipboard, never the
        // system one. Offscreen may have no selection clipboard at all.
        QCOMPARE(Clipboard()->text(), KEPT);
        if (Clipboard()->supportsSelection()) {
            QCOMPARE(Clipboard()->text(QClipboard::Selection),
                     QStringLiteral("hello"));
        }
#else
        // copy-on-select = none.
        QCOMPARE(Clipboard()->text(), KEPT);
#endif
    }

    void TheTerminalDroppingTheSelectionIsSeenByTheItem() {
        auto session = NewSession();
        Write(*session, "hello world");
        Show(session);
        Drag(CellLeft(0, 0), CellRight(4, 0));
        QVERIFY(Item_->hasSelection());
        QSignalSpy changed(Item_, &TerminalItem::hasSelectionChanged);

        // Erasing the screen erases the selection with it.
        Write(*Session_,
              "\x1b"
              "c");
        QVERIFY(DrawFrame());
        QVERIFY(QTest::qWaitFor([&] { return !Item_->hasSelection(); },
                                HAPPENS_MS));
        QCOMPARE(changed.count(), 1);
    }

private:
    QQuickWindow Window_;
    TerminalItem* Item_ = nullptr;
    std::shared_ptr<TerminalSession> Session_;
    roost::CellMetrics Cell_;
};

QTEST_MAIN(TerminalItemSelectionTest)
#include "terminal_item_selection_test.moc"
