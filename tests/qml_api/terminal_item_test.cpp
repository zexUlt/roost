// A TerminalItem in a real QQuickWindow on the offscreen platform: what a
// frame rebuilds, what it leaves clean, and that a background reaches the
// pixels. Glyph pixels are never compared: fonts differ between machines,
// backgrounds do not.

#include "qml_api/terminal_item.hpp"
#include "terminal/renderer.hpp"
#include "terminal/terminal_session.hpp"

#include <QColor>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>

using roost::TerminalItem;
using roost::TerminalSession;
using roost::VtRenderState;

namespace {

constexpr quint16 COLUMNS = 20;
constexpr quint16 ROWS = 8;
constexpr int FRAME_TIMEOUT_MS = 10000;
// Same as renderer.cpp's CURSOR_ALPHA.
constexpr int CURSOR_ALPHA = 160;
constexpr int TOLERANCE = 2;

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

// A rectangle of cells, in cell units: columns [column, column + columns) of
// one row.
struct CellRange {
    int Row = 0;
    int Column = 0;
    int Columns = 1;
};

QColor ToQColor(VtRenderState::Rgb rgb) { return QColor(rgb.R, rgb.G, rgb.B); }

int Distance(const QColor& a, const QColor& b) {
    return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) +
           std::abs(a.blue() - b.blue());
}

// What a block cursor of the renderer's alpha looks like over the default
// background. Same constant as renderer.cpp's CURSOR_ALPHA.
QColor CursorColor(const VtRenderState& state) {
    const auto fg = state.DefaultForeground();
    const auto bg = state.DefaultBackground();
    const auto mix = [](int f, int b) {
        return (f * CURSOR_ALPHA + b * (255 - CURSOR_ALPHA)) / 255;
    };
    return QColor(mix(fg.R, bg.R), mix(fg.G, bg.G), mix(fg.B, bg.B));
}

// Pixels of `image` inside `range` (grown by `slack` pixels on the sides) that
// are `color`, give or take `tolerance`; or, with `glyph` set, that are nearer
// to `color` than halfway from `background`: antialiased text never reaches
// the pure colour everywhere, but its core is past halfway.
struct Probe {
    QImage Image;
    int CellWidth = 0;
    int CellHeight = 0;

    int Count(const CellRange& range, const QColor& color, int tolerance,
              int slack = 0, const QColor* background = nullptr) const {
        const auto ratio = Image.devicePixelRatio();
        const int left = std::max(
            0, static_cast<int>((range.Column * CellWidth - slack) * ratio));
        const int right = std::min(
            Image.width(),
            static_cast<int>(
                ((range.Column + range.Columns) * CellWidth + slack) * ratio));
        const int top = static_cast<int>(range.Row * CellHeight * ratio);
        const int bottom =
            std::min(Image.height(),
                     static_cast<int>((range.Row + 1) * CellHeight * ratio));
        int found = 0;
        for (int y = top; y < bottom; ++y) {
            for (int x = left; x < right; ++x) {
                const auto pixel = Image.pixelColor(x, y);
                const bool hit = background == nullptr
                                     ? Distance(pixel, color) <= tolerance
                                     : Distance(pixel, color) * 2 <
                                           Distance(*background, color);
                found += hit ? 1 : 0;
            }
        }
        return found;
    }

    // Rightmost x, in cells scaled back from pixels, of a glyph pixel in the
    // row; -1 when there is none.
    int RightmostGlyphPixel(int row, const QColor& color,
                            const QColor& background) const {
        const auto ratio = Image.devicePixelRatio();
        const int top = static_cast<int>(row * CellHeight * ratio);
        const int bottom = std::min(
            Image.height(), static_cast<int>((row + 1) * CellHeight * ratio));
        int rightmost = -1;
        for (int y = top; y < bottom; ++y) {
            for (int x = 0; x < Image.width(); ++x) {
                if (Distance(Image.pixelColor(x, y), color) * 2 <
                    Distance(background, color)) {
                    rightmost = std::max(rightmost, x);
                }
            }
        }
        return rightmost < 0 ? -1 : static_cast<int>(rightmost / ratio);
    }
};

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

    void Show(std::shared_ptr<TerminalSession> session,
              const QFont* font = nullptr) {
        Session_ = std::move(session);
        Item_ = new TerminalItem(Window_.contentItem());
        if (font != nullptr) {
            Item_->setFont(*font);
        }
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

    Probe Grab() {
        const auto cell = roost::MeasureCell(Item_->font());
        Probe probe{.Image = Window_.grabWindow(),
                    .CellWidth = cell.Width,
                    .CellHeight = cell.Height};
        if (probe.Image.isNull()) {
            qFatal("grabWindow gave no image");
        }
        return probe;
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

    void GlyphColorReachesThePixels() {
        auto session = NewSession();
        // Pure red text on the default background, cursor hidden so that
        // nothing else draws near it. Row 2, columns 3 to 5.
        Write(*session, "\x1b[?25l\x1b[3;4H\x1b[38;2;255;0;0mHHH\x1b[0m");
        Show(session);

        const auto probe = Grab();
        const QColor red(255, 0, 0);
        const auto background =
            ToQColor(Session_->RenderState().DefaultBackground());
        QVERIFY(Distance(background, red) > 200);
        QVERIFY(probe.Count({2, 3, 3}, red, 0, 0, &background) > 0);
        QCOMPARE(probe.Count({2, 0, 3}, red, 0, 0, &background), 0);
        QCOMPARE(probe.Count({2, 6, 3}, red, 0, 0, &background), 0);
        QCOMPARE(probe.Count({1, 3, 3}, red, 0, 0, &background), 0);
    }

    void CursorFollowsTheTerminal() {
        auto session = NewSession();
        Show(session);

        const auto color = CursorColor(Session_->RenderState());
        const auto background =
            ToQColor(Session_->RenderState().DefaultBackground());
        QVERIFY(Distance(color, background) > 40);

        auto probe = Grab();
        QVERIFY(probe.Count({0, 0, 1}, color, TOLERANCE) > 0);
        QCOMPARE(probe.Count({4, 9, 1}, color, TOLERANCE), 0);

        Write(*Session_, "\x1b[5;10H");
        QVERIFY(DrawFrame());
        probe = Grab();
        QVERIFY(probe.Count({4, 9, 1}, color, TOLERANCE) > 0);
        QCOMPARE(probe.Count({0, 0, 1}, color, TOLERANCE), 0);

        Write(*Session_, "\x1b[?25l");
        QVERIFY(DrawFrame());
        probe = Grab();
        QCOMPARE(probe.Count({4, 9, 1}, color, TOLERANCE), 0);
        QCOMPARE(probe.Count({0, 0, 1}, color, TOLERANCE), 0);
    }

    void CursorCoversBothCellsOfAWideCharacter() {
        auto session = NewSession();
        // A CJK character in the first two cells, the cursor on its head.
        Write(*session, "\xe4\xb8\xad\x1b[1;1H");
        Show(session);

        const auto color = CursorColor(Session_->RenderState());
        auto probe = Grab();
        QVERIFY(probe.Count({0, 0, 1}, color, TOLERANCE) > 0);
        QVERIFY(probe.Count({0, 1, 1}, color, TOLERANCE) > 0);
        QCOMPARE(probe.Count({0, 2, 1}, color, TOLERANCE), 0);

        // On the tail the block still starts at the head.
        Write(*Session_, "\x1b[1;2H");
        QVERIFY(DrawFrame());
        QCOMPARE(Session_->RenderState().GetCursor().WideTail, true);
        probe = Grab();
        QVERIFY(probe.Count({0, 0, 1}, color, TOLERANCE) > 0);
        QVERIFY(probe.Count({0, 1, 1}, color, TOLERANCE) > 0);
        QCOMPARE(probe.Count({0, 2, 1}, color, TOLERANCE), 0);
    }

    void NewFontRebuildsEveryRow() {
        Show(NewSession());
        const auto before = roost::MeasureCell(Item_->font());
        QFont bigger = Item_->font();
        bigger.setPointSizeF(bigger.pointSizeF() * 2);
        Item_->setFont(bigger);
        QVERIFY(DrawFrame());
        QCOMPARE(Item_->rowsRebuilt(), int{ROWS});
        QVERIFY(Item_->cellWidth() > before.Width);
        QVERIFY(Item_->cellHeight() > before.Height);
    }

    void GlyphsStayOnTheGridWhenTheAdvanceIsFractional() {
        // A monospace face whose advance is not a whole number of pixels:
        // the cell is rounded up, and without the letter spacing nine glyphs
        // fall several pixels short of column 9. (A proportional face cannot
        // be pinned to the grid inside a run by spacing, so it is not used.)
        // The system's FixedFont alias may resolve to a proportional face on
        // some platforms (it does offscreen here), so a real fixed-pitch
        // family is looked up by name.
        QFont font;
        bool found = false;
        for (const auto& family : QFontDatabase::families()) {
            if (QFontDatabase::isFixedPitch(family)) {
                font = QFont(family);
                const QFontMetricsF metrics(font);
                found = metrics.horizontalAdvance(u'i') ==
                        metrics.horizontalAdvance(u'W');
                if (found) {
                    break;
                }
            }
        }
        if (!found) {
            QSKIP("no fixed-pitch font family on this system");
        }
        found = false;
        for (qreal size = 9; size <= 18 && !found; size += 0.1) {
            font.setPointSizeF(size);
            const auto advance = QFontMetricsF(font).horizontalAdvance(u'M');
            const auto fraction = advance - std::floor(advance);
            found = fraction >= 0.1 && fraction <= 0.4;
        }
        if (!found) {
            QSKIP("no size of the monospace font has a fractional advance");
        }
        auto session = NewSession();
        Write(*session, "\x1b[?25l\x1b[38;2;255;0;0miiiiiiiiiX\x1b[0m");
        Show(session, &font);

        const auto probe = Grab();
        const QColor red(255, 0, 0);
        const auto background =
            ToQColor(Session_->RenderState().DefaultBackground());
        // The X is the last glyph of the run: it is in column 9, give or take
        // a pixel of antialiasing, and nothing of the text lies past it.
        QVERIFY(probe.Count({0, 9, 1}, red, 0, 1, &background) > 0);
        QCOMPARE(probe.Count({0, 10, 5}, red, 0, 1, &background), 0);
        const int rightmost = probe.RightmostGlyphPixel(0, red, background);
        QVERIFY(rightmost < 10 * probe.CellWidth + 1);
        QVERIFY(rightmost >= 9 * probe.CellWidth);
    }

    void ResizeOfColumnsOnlyRebuildsEveryRow() {
        auto session = NewSession();
        // The whole screen red, so that a row left over shows.
        Write(*session, "\x1b[?25l\x1b[41m\x1b[2J\x1b[0m");
        Show(session);
        {
            const auto lock = Session_->Lock();
            QVERIFY(Session_->Resize(lock, 12, ROWS));
        }
        QVERIFY(DrawFrame());
        QCOMPARE(Item_->rowsRebuilt(), int{ROWS});
        QCOMPARE(Session_->RenderState().Columns(), quint16{12});

        const auto probe = Grab();
        const QColor magenta(Qt::magenta);
        for (int row = 0; row < ROWS; ++row) {
            QCOMPARE(probe.Count({row, 14, 1}, magenta, 0),
                     probe.CellWidth * probe.CellHeight *
                         static_cast<int>(probe.Image.devicePixelRatio() *
                                          probe.Image.devicePixelRatio()));
        }
    }

private:
    QQuickWindow Window_;
    TerminalItem* Item_ = nullptr;
    std::shared_ptr<TerminalSession> Session_;
};

QTEST_MAIN(TerminalItemTest)
#include "terminal_item_test.moc"
