// A TerminalItem in a real QQuickWindow on the offscreen platform: what a
// frame rebuilds, what it leaves clean, and that a background reaches the
// pixels. Glyph pixels are never compared: fonts differ between machines,
// backgrounds do not.

#include "qml_api/terminal_item.hpp"
#include "terminal/renderer.hpp"
#include "terminal/terminal_session.hpp"

#include <QColor>
#include <QImage>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>

#include <memory>

using roost::TerminalItem;
using roost::TerminalSession;
using roost::VtRenderState;

namespace {

constexpr quint16 COLUMNS = 20;
constexpr quint16 ROWS = 8;
constexpr int FRAME_TIMEOUT_MS = 10000;

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

}  // namespace

class TerminalItemTest : public QObject {
    Q_OBJECT

private:
    // One frame, drawn: asks for it and waits for the swap.
    bool DrawFrame() {
        QSignalSpy swapped(&Window_, &QQuickWindow::frameSwapped);
        Item_->update();
        return swapped.wait(FRAME_TIMEOUT_MS);
    }

    void Show(std::shared_ptr<TerminalSession> session) {
        Session_ = std::move(session);
        Item_ = new TerminalItem(Window_.contentItem());
        Item_->setSession(Session_);
        const auto cell = roost::MeasureCell(Item_->font());
        Item_->setSize(QSizeF(COLUMNS * cell.Width, ROWS * cell.Height));
        Window_.resize(COLUMNS * cell.Width, ROWS * cell.Height);
        QSignalSpy swapped(&Window_, &QQuickWindow::frameSwapped);
        Window_.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Window_));
        // The first frame comes with the exposure, not with a request.
        QVERIFY(swapped.count() > 0 || swapped.wait(FRAME_TIMEOUT_MS));
    }

private slots:
    void init() {
        // A colour no cell has, so a hole in the frame shows.
        Window_.setColor(Qt::magenta);
    }

    void cleanup() {
        Window_.hide();
        delete Item_;
        Item_ = nullptr;
        Session_.reset();
    }

    void FirstFrameRebuildsEveryRowThenOnlyTheDirtyOnes() {
        auto session = NewSession();
        // The cursor goes to row 3 before the first frame, so that the write
        // below moves it nowhere.
        Write(*session, "\x1b[4;1Hx");
        Show(session);
        QCOMPARE(Item_->rowsRebuilt(), int{ROWS});
        QCOMPARE(Session_->RenderState().Dirty(), VtRenderState::EDirty::None);

        Write(*Session_, "y");
        QVERIFY(DrawFrame());
        QCOMPARE(Item_->rowsRebuilt(), 1);
        QCOMPARE(Session_->RenderState().Dirty(), VtRenderState::EDirty::None);

        // Nothing written: nothing rebuilt.
        QVERIFY(DrawFrame());
        QCOMPARE(Item_->rowsRebuilt(), 0);
    }

    void BackgroundReachesThePixels() {
        auto session = NewSession();
        // Row 3, column 5: a red background on one blank cell.
        Write(*session, "\x1b[4;6H\x1b[41m \x1b[0m");
        Show(session);

        const auto cell = roost::MeasureCell(Item_->font());
        const auto rows = Session_->RenderState().Rows();
        const auto red = rows[3].Cells[5].Background;
        QVERIFY(red.has_value());
        const auto defaultBackground =
            Session_->RenderState().DefaultBackground();
        QVERIFY(*red != defaultBackground);

        const auto image = Window_.grabWindow();
        QVERIFY(!image.isNull());
        const auto ratio = image.devicePixelRatio();
        const auto pixelAt = [&](int column, int row) {
            return image
                .pixelColor(
                    static_cast<int>((column + 0.5) * cell.Width * ratio),
                    static_cast<int>((row + 0.5) * cell.Height * ratio))
                .rgb();
        };
        QCOMPARE(pixelAt(5, 3), qRgb(red->R, red->G, red->B));
        // The neighbours are the default background; the cursor, which sits
        // right after the cell, is not.
        const auto plain =
            qRgb(defaultBackground.R, defaultBackground.G, defaultBackground.B);
        QCOMPARE(pixelAt(4, 3), plain);
        QCOMPARE(pixelAt(7, 3), plain);
        QCOMPARE(pixelAt(5, 2), plain);
    }

    void SwitchingSessionsRedraws() {
        // The same cell is red in A and green in B.
        auto a = NewSession();
        Write(*a, "\x1b[4;6H\x1b[41m \x1b[0m");
        auto b = NewSession();
        Write(*b, "\x1b[4;6H\x1b[42m \x1b[0m");
        Show(a);

        const auto cell = roost::MeasureCell(Item_->font());
        const auto sample = [&] {
            const auto image = Window_.grabWindow();
            const auto ratio = image.devicePixelRatio();
            return image
                .pixelColor(static_cast<int>(5.5 * cell.Width * ratio),
                            static_cast<int>(3.5 * cell.Height * ratio))
                .rgb();
        };
        const auto colorOf = [&](TerminalSession& session) {
            const auto background =
                session.RenderState().Rows()[3].Cells[5].Background;
            if (!background) {
                qFatal("no background");
            }
            return qRgb(background->R, background->G, background->B);
        };
        const auto red = colorOf(*a);
        QCOMPARE(sample(), red);

        // B has never been drawn, but A has been: the render state of A is
        // clean when A comes back.
        Item_->setSession(b);
        Session_ = b;
        QVERIFY(DrawFrame());
        QCOMPARE(Item_->rowsRebuilt(), int{ROWS});
        const auto green = colorOf(*b);
        QVERIFY(green != red);
        QCOMPARE(sample(), green);

        Item_->setSession(a);
        Session_ = a;
        QVERIFY(DrawFrame());
        QCOMPARE(Item_->rowsRebuilt(), int{ROWS});
        QCOMPARE(sample(), red);
    }

    void ResizeRebuildsEveryRow() {
        Show(NewSession());
        {
            const auto lock = Session_->Lock();
            QVERIFY(Session_->Resize(lock, 30, 5));
        }
        QVERIFY(DrawFrame());
        QCOMPARE(Item_->rowsRebuilt(), 5);
        QCOMPARE(Session_->RenderState().Dirty(), VtRenderState::EDirty::None);
        QCOMPARE(Session_->RenderState().Columns(), quint16{30});

        // The rows the smaller grid no longer has are gone from the scene:
        // what is left there is the window's own colour, not a stale row.
        const auto cell = roost::MeasureCell(Item_->font());
        const auto image = Window_.grabWindow();
        const auto ratio = image.devicePixelRatio();
        const auto stale =
            image.pixelColor(static_cast<int>(0.5 * cell.Width * ratio),
                             static_cast<int>(6.5 * cell.Height * ratio));
        QCOMPARE(stale.rgb(), QColor(Qt::magenta).rgb());
    }

private:
    QQuickWindow Window_;
    TerminalItem* Item_ = nullptr;
    std::shared_ptr<TerminalSession> Session_;
};

QTEST_MAIN(TerminalItemTest)
#include "terminal_item_test.moc"
