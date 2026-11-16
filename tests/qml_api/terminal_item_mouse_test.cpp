// A focused TerminalItem in a real QQuickWindow on the offscreen platform:
// what the mouse does. With the program tracking it, clicks, drags, motion
// and the wheel leave as the reports it asked for, at the cell under the
// pointer. Without, the wheel looks through the scrollback of the primary
// screen, and on the alternate screen types cursor keys when the program
// turned alternate scroll on, as Ghostty does. Trackpad movement in pixels
// adds up to whole rows. Rows are read from the render state of a frame the
// window drew, so a move the item did not ask a frame for shows.

#include "qml_api/terminal_item.hpp"
#include "terminal/qt_key_mapping.hpp"
#include "terminal/renderer.hpp"
#include "terminal/terminal_session.hpp"

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
// The frame the item asked for comes within a few milliseconds.
constexpr int ASKED_FRAME_MS = 3000;
// A frame nobody asked for does not come; this is how long that is awaited.
constexpr int NO_FRAME_MS = 300;
// Qt counts a notch of a wheel as 120 units; the item moves three rows for it,
// Ghostty's default for a wheel.
constexpr int NOTCH = 120;

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

// "L00\r\nL01\r\n ... L29": the last LAST_LINE - ROWS + 2 lines are on the
// screen, the rest in the scrollback.
constexpr int LAST_LINE = 29;

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

}  // namespace

class TerminalItemMouseTest : public QObject {
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

    // The middle of a cell, in window coordinates.
    QPoint CellCenter(int column, int row) const {
        return {column * Cell_.Width + Cell_.Width / 2,
                row * Cell_.Height + Cell_.Height / 2};
    }

    // The text of a row of the last frame, without the blanks at the end.
    QString RowText(int row) const {
        QString text;
        for (const auto& cell : Session_->RenderState().Rows()[row].Cells) {
            text += cell.Text.isEmpty() ? QStringLiteral(" ") : cell.Text;
        }
        return text.trimmed();
    }

    // Waits for the frame the item asked for and says what its top row is.
    // Fails the test if there is none.
    QString TopRowAfterFrame() {
        QSignalSpy swapped(&Window_, &QQuickWindow::frameSwapped);
        if (!swapped.wait(ASKED_FRAME_MS)) {
            QTest::qFail("the item asked for no frame", __FILE__, __LINE__);
            return {};
        }
        return RowText(0);
    }

    bool AtBottom() const {
        const auto lock = Session_->Lock();
        return Session_->ViewportAtBottom(lock).value_or(false);
    }

    void SendWheel(const QPoint& at, QPoint angle, QPoint pixels = {},
                   Qt::ScrollPhase phase = Qt::NoScrollPhase,
                   Qt::MouseButtons buttons = Qt::NoButton) {
        QWheelEvent event(QPointF(at), QPointF(Window_.mapToGlobal(at)), pixels,
                          angle, buttons, {}, phase, false);
        event.setAccepted(false);
        QCoreApplication::sendEvent(&Window_, &event);
    }

    void Notch(int count = 1, const QPoint& at = {5, 5}) {
        SendWheel(at, QPoint(0, NOTCH * count));
    }

    // A primary screen with 22 lines of scrollback: the screen shows L22 to
    // L29.
    void ShowWithScrollback() {
        auto session = NewSession();
        Write(*session, Lines(0, LAST_LINE));
        Show(session);
        QCOMPARE(RowText(0), QStringLiteral("L22"));
        QCOMPARE(RowText(ROWS - 1), QStringLiteral("L29"));
    }

private slots:
    void cleanup() {
        Window_.hide();
        delete Item_;
        Item_ = nullptr;
        Session_.reset();
    }

    void ClickIsReportedAtTheCellUnderThePointer() {
        Show(NewSession());
        // Normal tracking, SGR format.
        Write(*Session_, "\x1b[?1000h\x1b[?1006h");
        QSignalSpy input(Item_, &TerminalItem::input);

        QTest::mousePress(&Window_, Qt::LeftButton, {}, CellCenter(3, 2));
        QCOMPARE(input.count(), 1);
        QCOMPARE(input.last().at(0).toByteArray(), QByteArray("\x1b[<0;4;3M"));
        QTest::mouseRelease(&Window_, Qt::LeftButton, {}, CellCenter(3, 2));
        QCOMPARE(input.count(), 2);
        QCOMPARE(input.last().at(0).toByteArray(), QByteArray("\x1b[<0;4;3m"));

        // Another button, another cell, a modifier.
        QTest::mouseClick(&Window_, Qt::RightButton, Qt::AltModifier,
                          CellCenter(10, 5));
        QCOMPARE(input.count(), 4);
        QCOMPARE(input.at(2).at(0).toByteArray(), QByteArray("\x1b[<10;11;6M"));
        QCOMPARE(input.at(3).at(0).toByteArray(), QByteArray("\x1b[<10;11;6m"));
    }

    void ClickInTheLegacyFormat() {
        Show(NewSession());
        Write(*Session_, "\x1b[?1000h");
        QSignalSpy input(Item_, &TerminalItem::input);

        QTest::mouseClick(&Window_, Qt::LeftButton, {}, CellCenter(3, 2));
        QCOMPARE(input.count(), 2);
        QCOMPARE(input.at(0).at(0).toByteArray(), QByteArray("\x1b[M $#"));
        QCOMPARE(input.at(1).at(0).toByteArray(), QByteArray("\x1b[M#$#"));
    }

    void NothingIsReportedWhileTheProgramDoesNotTrack() {
        ShowWithScrollback();
        Write(*Session_, "\x1b[?1006h");
        QSignalSpy input(Item_, &TerminalItem::input);

        QTest::mouseClick(&Window_, Qt::LeftButton, {}, CellCenter(3, 2));
        QTest::mouseMove(&Window_, CellCenter(4, 3));
        QTest::mouseClick(&Window_, Qt::RightButton, {}, CellCenter(5, 4));
        QCOMPARE(input.count(), 0);

        // Tracking on and off again: off means off.
        Write(*Session_, "\x1b[?1000h\x1b[?1000l");
        QTest::mouseClick(&Window_, Qt::LeftButton, {}, CellCenter(3, 2));
        QCOMPARE(input.count(), 0);
    }

    void DragIsReportedInButtonMode() {
        Show(NewSession());
        Write(*Session_, "\x1b[?1002h\x1b[?1006h");
        QSignalSpy input(Item_, &TerminalItem::input);

        QTest::mousePress(&Window_, Qt::LeftButton, {}, CellCenter(3, 2));
        QTest::mouseMove(&Window_, CellCenter(4, 2));
        QTest::mouseMove(&Window_, CellCenter(5, 3));
        QTest::mouseRelease(&Window_, Qt::LeftButton, {}, CellCenter(5, 3));
        QCOMPARE(input.count(), 4);
        QCOMPARE(input.at(0).at(0).toByteArray(), QByteArray("\x1b[<0;4;3M"));
        QCOMPARE(input.at(1).at(0).toByteArray(), QByteArray("\x1b[<32;5;3M"));
        QCOMPARE(input.at(2).at(0).toByteArray(), QByteArray("\x1b[<32;6;4M"));
        QCOMPARE(input.at(3).at(0).toByteArray(), QByteArray("\x1b[<0;6;4m"));

        // With no button held, button mode does not report a move.
        QTest::mouseMove(&Window_, CellCenter(8, 3));
        QCOMPARE(input.count(), 4);
    }

    void MotionWithoutAButtonIsReportedInAnyEventMode() {
        Show(NewSession());
        Write(*Session_, "\x1b[?1000h\x1b[?1006h");
        QSignalSpy input(Item_, &TerminalItem::input);

        // Normal mode has no motion.
        QTest::mouseMove(&Window_, CellCenter(2, 2));
        QTest::mouseMove(&Window_, CellCenter(3, 2));
        QCOMPARE(input.count(), 0);

        Write(*Session_, "\x1b[?1003h");
        QTest::mouseMove(&Window_, CellCenter(4, 2));
        QCOMPARE(input.count(), 1);
        QCOMPARE(input.last().at(0).toByteArray(), QByteArray("\x1b[<35;5;3M"));
        // The same cell again says nothing new.
        QTest::mouseMove(&Window_, CellCenter(4, 2) + QPoint(1, 1));
        QCOMPARE(input.count(), 1);
        QTest::mouseMove(&Window_, CellCenter(5, 2));
        QCOMPARE(input.count(), 2);
        QCOMPARE(input.last().at(0).toByteArray(), QByteArray("\x1b[<35;6;3M"));
    }

    void WheelIsReportedWhenTheProgramTracksTheMouse() {
        ShowWithScrollback();
        Write(*Session_, "\x1b[?1000h\x1b[?1006h");
        QSignalSpy input(Item_, &TerminalItem::input);

        // A notch up is three presses of button 4, at the pointer; the
        // viewport does not move.
        Notch(1, CellCenter(3, 2));
        QCOMPARE(input.count(), 1);
        QCOMPARE(input.last().at(0).toByteArray(),
                 QByteArray("\x1b[<64;4;3M\x1b[<64;4;3M\x1b[<64;4;3M"));
        SendWheel(CellCenter(3, 2), QPoint(0, -NOTCH));
        QCOMPARE(input.last().at(0).toByteArray(),
                 QByteArray("\x1b[<65;4;3M\x1b[<65;4;3M\x1b[<65;4;3M"));
        QVERIFY(AtBottom());
    }

    void WheelOnThePrimaryScreenScrollsTheViewportAndBack() {
        ShowWithScrollback();
        QSignalSpy input(Item_, &TerminalItem::input);

        Notch();
        QCOMPARE(TopRowAfterFrame(), QStringLiteral("L19"));
        QVERIFY(!AtBottom());
        Notch(2);
        QCOMPARE(TopRowAfterFrame(), QStringLiteral("L13"));

        // Down, past where it started: it stops at the bottom.
        SendWheel({5, 5}, QPoint(0, -NOTCH * 2));
        QCOMPARE(TopRowAfterFrame(), QStringLiteral("L19"));
        SendWheel({5, 5}, QPoint(0, -NOTCH * 4));
        QCOMPARE(TopRowAfterFrame(), QStringLiteral("L22"));
        QVERIFY(AtBottom());

        // All of it without a byte to the program.
        QCOMPARE(input.count(), 0);
    }

    void WheelStopsAtTheTopOfTheScrollback() {
        ShowWithScrollback();
        Notch(30);
        QCOMPARE(TopRowAfterFrame(), QStringLiteral("L00"));
    }

    void TheCursorIsNotDrawnOutsideTheViewport() {
        ShowWithScrollback();
        // The cursor is on the last row, right after "L29".
        const auto cursorCell = [&] {
            const auto image = Window_.grabWindow();
            const auto ratio = image.devicePixelRatio();
            return image
                .pixelColor(
                    static_cast<int>(3.5 * Cell_.Width * ratio),
                    static_cast<int>((ROWS - 0.5) * Cell_.Height * ratio))
                .rgb();
        };
        const auto background = Session_->RenderState().DefaultBackground();
        const auto plain = qRgb(background.R, background.G, background.B);
        QVERIFY(Session_->RenderState().GetCursor().InViewport);
        QVERIFY(cursorCell() != plain);

        Notch(30);
        QCOMPARE(TopRowAfterFrame(), QStringLiteral("L00"));
        QVERIFY(!Session_->RenderState().GetCursor().InViewport);
        // The rows there are "L00" to "L07": blank at that cell, and no
        // block over it, nor in the corner a cursor with no place would
        // fall back to.
        QCOMPARE(cursorCell(), plain);
        QCOMPARE(Window_.grabWindow().pixelColor(1, 1).rgb(), plain);
    }

    void OutputDoesNotPullTheViewportBack() {
        ShowWithScrollback();
        Notch();
        QCOMPARE(TopRowAfterFrame(), QStringLiteral("L19"));

        // New lines while the user reads the history: what is on the screen
        // stays (Ghostty's default, scroll-to-bottom without `output`).
        Write(*Session_, "\r\n" + Lines(30, 33));
        Item_->update();
        QCOMPARE(TopRowAfterFrame(), QStringLiteral("L19"));
        QVERIFY(!AtBottom());
    }

    void TypingScrollsToTheBottom() {
        ShowWithScrollback();
        QSignalSpy input(Item_, &TerminalItem::input);
        Notch();
        QCOMPARE(TopRowAfterFrame(), QStringLiteral("L19"));

        QKeyEvent press(QEvent::KeyPress, Qt::Key_A, {}, "a");
        QCoreApplication::sendEvent(&Window_, &press);
        QCOMPARE(input.count(), 1);
        QCOMPARE(TopRowAfterFrame(), QStringLiteral("L22"));
        QVERIFY(AtBottom());
    }

    void AKeyThatTypesNothingKeepsTheViewport() {
        ShowWithScrollback();
        Notch();
        QCOMPARE(TopRowAfterFrame(), QStringLiteral("L19"));

        // A modifier alone sends nothing, and moves nothing.
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
        QCoreApplication::sendEvent(&Window_, &press);
        QVERIFY(!AtBottom());
    }

    void PastingScrollsToTheBottom() {
        ShowWithScrollback();
        QSignalSpy input(Item_, &TerminalItem::input);
        Notch();
        QCOMPARE(TopRowAfterFrame(), QStringLiteral("L19"));

        Item_->paste("x");
        QCOMPARE(input.count(), 1);
        QCOMPARE(TopRowAfterFrame(), QStringLiteral("L22"));
        QVERIFY(AtBottom());
    }

    void WheelOnTheAlternateScreenWithAlternateScrollTypesCursorKeys() {
        ShowWithScrollback();
        // The alternate screen, and mode 1007.
        Write(*Session_, "\x1b[?1049h\x1b[?1007h");
        QSignalSpy input(Item_, &TerminalItem::input);

        Notch();
        QCOMPARE(input.count(), 1);
        QCOMPARE(input.last().at(0).toByteArray(),
                 QByteArray("\x1b[A\x1b[A\x1b[A"));
        SendWheel({5, 5}, QPoint(0, -NOTCH));
        QCOMPARE(input.last().at(0).toByteArray(),
                 QByteArray("\x1b[B\x1b[B\x1b[B"));

        // As the program's cursor key mode says.
        Write(*Session_, "\x1b[?1h");
        Notch();
        QCOMPARE(input.last().at(0).toByteArray(),
                 QByteArray("\x1bOA\x1bOA\x1bOA"));
        QCOMPARE(input.count(), 3);
    }

    void AlternateScrollKeysAreFixedBytesEvenUnderTheKittyProtocol() {
        ShowWithScrollback();
        // CSI > 31 u: the kitty keyboard protocol, all flags.
        Write(*Session_, "\x1b[?1049h\x1b[?1007h\x1b[>31u");
        QSignalSpy input(Item_, &TerminalItem::input);

        Notch();
        QCOMPARE(input.last().at(0).toByteArray(),
                 QByteArray("\x1b[A\x1b[A\x1b[A"));
        Write(*Session_, "\x1b[?1h");
        SendWheel({5, 5}, QPoint(0, -NOTCH));
        QCOMPARE(input.last().at(0).toByteArray(),
                 QByteArray("\x1bOB\x1bOB\x1bOB"));
    }

    void WheelOnTheAlternateScreenWithoutAlternateScrollDoesNothing() {
        ShowWithScrollback();
        Write(*Session_, "\x1b[?1049h\x1b[?1007l");
        QSignalSpy input(Item_, &TerminalItem::input);

        Notch();
        QCOMPARE(input.count(), 0);
        QVERIFY(AtBottom());
    }

    void TrackingBeatsAlternateScroll() {
        ShowWithScrollback();
        Write(*Session_, "\x1b[?1049h\x1b[?1007h\x1b[?1000h\x1b[?1006h");
        QSignalSpy input(Item_, &TerminalItem::input);

        Notch(1, CellCenter(1, 1));
        QCOMPARE(input.count(), 1);
        QVERIFY(input.last().at(0).toByteArray().startsWith("\x1b[<64;2;2M"));
    }

    void AlternateScrollOnThePrimaryScreenStillScrolls() {
        ShowWithScrollback();
        Write(*Session_, "\x1b[?1007h");
        QSignalSpy input(Item_, &TerminalItem::input);

        Notch();
        QCOMPARE(TopRowAfterFrame(), QStringLiteral("L19"));
        QCOMPARE(input.count(), 0);
    }

    void TrackpadPixelsAddUpToWholeRows() {
        ShowWithScrollback();
        QSignalSpy input(Item_, &TerminalItem::input);
        // A third of a row, rounded up: two of them are less than a row,
        // three are more.
        const int third = (Cell_.Height + 2) / 3;
        QVERIFY(2 * third < Cell_.Height);
        QVERIFY(3 * third >= Cell_.Height);

        SendWheel({5, 5}, QPoint(0, 0), QPoint(0, third));
        SendWheel({5, 5}, QPoint(0, 0), QPoint(0, third));
        QVERIFY(AtBottom());
        QSignalSpy swapped(&Window_, &QQuickWindow::frameSwapped);
        QVERIFY(!swapped.wait(NO_FRAME_MS));

        SendWheel({5, 5}, QPoint(0, 0), QPoint(0, third));
        QCOMPARE(TopRowAfterFrame(), QStringLiteral("L21"));

        // What was left over is not lost, and not repeated: the next third
        // is still short of a row.
        SendWheel({5, 5}, QPoint(0, 0), QPoint(0, third));
        QSignalSpy again(&Window_, &QQuickWindow::frameSwapped);
        QVERIFY(!again.wait(NO_FRAME_MS));
        QCOMPARE(RowText(0), QStringLiteral("L21"));

        // A big pixel move is several rows at once, and down undoes up.
        SendWheel({5, 5}, QPoint(0, 0), QPoint(0, -Cell_.Height * 5));
        QCOMPARE(TopRowAfterFrame(), QStringLiteral("L22"));
        QCOMPARE(input.count(), 0);
    }

    void ModifiersAreInAClickReport() {
        Show(NewSession());
        Write(*Session_, "\x1b[?1000h\x1b[?1006h");
        QSignalSpy input(Item_, &TerminalItem::input);
        const auto ctrl =
            roost::QtKeyOptions::ForThisPlatform().SwapControlAndMeta
                ? Qt::MetaModifier
                : Qt::ControlModifier;

        QTest::mousePress(&Window_, Qt::LeftButton, Qt::AltModifier,
                          CellCenter(3, 2));
        QCOMPARE(input.last().at(0).toByteArray(), QByteArray("\x1b[<8;4;3M"));
        QTest::mouseRelease(&Window_, Qt::LeftButton, Qt::AltModifier,
                            CellCenter(3, 2));
        QCOMPARE(input.last().at(0).toByteArray(), QByteArray("\x1b[<8;4;3m"));
        QTest::mousePress(&Window_, Qt::LeftButton, ctrl, CellCenter(3, 2));
        QCOMPARE(input.last().at(0).toByteArray(), QByteArray("\x1b[<16;4;3M"));
        QTest::mouseRelease(&Window_, Qt::LeftButton, ctrl, CellCenter(3, 2));
        QCOMPARE(input.last().at(0).toByteArray(), QByteArray("\x1b[<16;4;3m"));
        QCOMPARE(input.count(), 4);
    }

    void WheelReportCarriesTheModifiers() {
        Show(NewSession());
        Write(*Session_, "\x1b[?1000h\x1b[?1006h");
        QSignalSpy input(Item_, &TerminalItem::input);
        QWheelEvent event(QPointF(CellCenter(3, 2)),
                          QPointF(Window_.mapToGlobal(CellCenter(3, 2))), {},
                          QPoint(0, NOTCH), Qt::NoButton, Qt::ShiftModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&Window_, &event);
        QCOMPARE(input.last().at(0).toByteArray(),
                 QByteArray("\x1b[<68;4;3M\x1b[<68;4;3M\x1b[<68;4;3M"));
    }

    void AWheelOutsideTheGridInAnyEventModeNeedsAHeldButton() {
        Show(NewSession());
        Write(*Session_, "\x1b[?1003h\x1b[?1006h");
        // Room past the last column that is no cell of the grid.
        const int wide = (COLUMNS + 1) * Cell_.Width - 1;
        Item_->setSize(QSizeF(wide, ROWS * Cell_.Height));
        Window_.resize(wide, ROWS * Cell_.Height);
        QCOMPARE(Item_->columns(), COLUMNS);
        QSignalSpy input(Item_, &TerminalItem::input);
        const QPoint outside(COLUMNS * Cell_.Width + 2, 5);

        SendWheel(outside, QPoint(0, NOTCH));
        QCOMPARE(input.count(), 0);
        // With a button held it is reported, at the edge.
        SendWheel(outside, QPoint(0, NOTCH), {}, Qt::NoScrollPhase,
                  Qt::LeftButton);
        QCOMPARE(input.count(), 1);
        QVERIFY(input.last().at(0).toByteArray().startsWith("\x1b[<64;20;1M"));
    }

    void ASidewaysNotchIsOneReportWithQtsPositiveXAsButtonSix() {
        ShowWithScrollback();
        Write(*Session_, "\x1b[?1000h\x1b[?1006h");
        QSignalSpy input(Item_, &TerminalItem::input);

        SendWheel(CellCenter(3, 2), QPoint(NOTCH, 0));
        QCOMPARE(input.count(), 1);
        QCOMPARE(input.last().at(0).toByteArray(), QByteArray("\x1b[<66;4;3M"));
        SendWheel(CellCenter(3, 2), QPoint(-NOTCH, 0));
        QCOMPARE(input.count(), 2);
        QCOMPARE(input.last().at(0).toByteArray(), QByteArray("\x1b[<67;4;3M"));
    }

    void SidewaysPixelsAddUpByCellWidth() {
        ShowWithScrollback();
        Write(*Session_, "\x1b[?1000h\x1b[?1006h");
        QSignalSpy input(Item_, &TerminalItem::input);

        SendWheel({5, 5}, QPoint(0, 0), QPoint(Cell_.Width - 1, 0));
        QCOMPARE(input.count(), 0);
        SendWheel({5, 5}, QPoint(0, 0), QPoint(1, 0));
        QCOMPARE(input.count(), 1);
        QCOMPARE(input.last().at(0).toByteArray().count("\x1b[<66;"), 1);
    }

    void ARemainderSurvivesTheStartOfAGesture() {
        ShowWithScrollback();
        QSignalSpy input(Item_, &TerminalItem::input);

        SendWheel({5, 5}, QPoint(0, 0), QPoint(0, Cell_.Height - 1));
        QVERIFY(AtBottom());
        // Ghostty never drops the remainder; the pixel that completes the
        // row counts even when a new gesture starts with it.
        SendWheel({5, 5}, QPoint(0, 0), QPoint(0, 1), Qt::ScrollBegin);
        QCOMPARE(TopRowAfterFrame(), QStringLiteral("L21"));
    }

    void TrackpadPixelsAreReportedByRowsWhenTheProgramTracks() {
        ShowWithScrollback();
        Write(*Session_, "\x1b[?1000h\x1b[?1006h");
        QSignalSpy input(Item_, &TerminalItem::input);

        SendWheel({5, 5}, QPoint(0, 0), QPoint(0, Cell_.Height - 1));
        QCOMPARE(input.count(), 0);
        SendWheel({5, 5}, QPoint(0, 0), QPoint(0, 1));
        QCOMPARE(input.count(), 1);
        QCOMPARE(input.last().at(0).toByteArray().count("\x1b[<64;"), 1);
    }

    void WithoutASessionTheMouseIsIgnored() {
        Item_ = new TerminalItem(Window_.contentItem());
        Item_->setSize(QSizeF(100, 100));
        Window_.resize(100, 100);
        Window_.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Window_));
        QSignalSpy input(Item_, &TerminalItem::input);

        QTest::mouseClick(&Window_, Qt::LeftButton, {}, QPoint(5, 5));
        QTest::mouseMove(&Window_, QPoint(20, 20));
        SendWheel({5, 5}, QPoint(0, NOTCH));
        QCOMPARE(input.count(), 0);
    }

private:
    QQuickWindow Window_;
    TerminalItem* Item_ = nullptr;
    roost::CellMetrics Cell_;
    std::shared_ptr<TerminalSession> Session_;
};

QTEST_MAIN(TerminalItemMouseTest)
#include "terminal_item_mouse_test.moc"
