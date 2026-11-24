// The part of a TerminalItem that no cell covers: the item fits a whole number
// of rows and columns, and what is left (less than a cell, below and to the
// right of the grid) is painted with the default background of the terminal,
// not left to whatever lies under the item. In a real QQuickWindow on the
// offscreen platform; the picture is the content item's (grabToImage). With
// ROOST_TEST_SNAPSHOTS set to a directory, the corner of the grid and the
// remainder is saved there, zoomed.

#include "core/terminal_theme.hpp"
#include "core/theme_source.hpp"
#include "qml_api/terminal_item.hpp"
#include "terminal/renderer.hpp"
#include "terminal/terminal_session.hpp"

#include <QColor>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>

#include <memory>

using roost::TerminalItem;
using roost::TerminalSession;

namespace {

constexpr quint16 COLUMNS = 20;
constexpr quint16 ROWS = 8;
constexpr int FRAME_TIMEOUT_MS = 10000;
// Pixels the item is longer and taller than the grid; less than a cell.
constexpr int EXTRA_WIDTH = 6;
constexpr int EXTRA_HEIGHT = 5;
// Dark, so that it is not the colour of the card or of the window.
const QColor DARK(0x10, 0x20, 0x30);
const QColor GREEN(0x20, 0x40, 0x10);

std::shared_ptr<TerminalSession> NewSession() {
    auto session = TerminalSession::Create(COLUMNS, ROWS);
    if (!session) {
        qFatal("%s", qPrintable(session.error()));
    }
    return std::shared_ptr<TerminalSession>(std::move(*session));
}

// The default theme's colours with another background, as the terminal panel
// sets them from the theme.
void SetBackground(TerminalSession& session, const QColor& color) {
    auto colors = roost::ParseTerminalColors(roost::ResolveTheme({}).Tokens);
    if (!colors) {
        qFatal("%s", qPrintable(colors.error()));
    }
    colors->Background = {.R = static_cast<quint8>(color.red()),
                          .G = static_cast<quint8>(color.green()),
                          .B = static_cast<quint8>(color.blue())};
    const auto lock = session.Lock();
    if (const auto set = session.SetColors(lock, *colors); !set) {
        qFatal("%s", qPrintable(set.error()));
    }
}

}  // namespace

class TerminalItemRemainderTest : public QObject {
    Q_OBJECT

private:
    // One frame: asks for it and waits for its sync, so that the render
    // state is the one this frame read.
    bool DrawFrame() {
        QSignalSpy synced(&Window_, &QQuickWindow::afterSynchronizing);
        Item_->update();
        return synced.wait(FRAME_TIMEOUT_MS);
    }

    // The item is the grid and a remainder, in the middle of a window that is
    // larger by a margin of the same colour as nothing the item paints.
    void Show(std::shared_ptr<TerminalSession> session) {
        Session_ = std::move(session);
        Item_ = new TerminalItem(Window_.contentItem());
        Item_->setSession(Session_);
        Cell_ = roost::MeasureCell(Item_->font());
        Item_->setPosition(QPointF(MARGIN, MARGIN));
        Item_->setSize(QSizeF(COLUMNS * Cell_.Width + EXTRA_WIDTH,
                              ROWS * Cell_.Height + EXTRA_HEIGHT));
        Window_.resize(static_cast<int>(Item_->width()) + 2 * MARGIN,
                       static_cast<int>(Item_->height()) + 2 * MARGIN);
        QSignalSpy swapped(&Window_, &QQuickWindow::frameSwapped);
        Window_.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Window_));
        // The first frame comes with the exposure, not with a request.
        QVERIFY(swapped.count() > 0 || swapped.wait(FRAME_TIMEOUT_MS));
    }

    // The picture of the content item, after the frame that drew the state.
    QImage Grab() {
        QSharedPointer<QQuickItemGrabResult> result =
            Window_.contentItem()->grabToImage();
        if (result.isNull()) {
            qFatal("grabToImage gave no result");
        }
        QSignalSpy ready(result.data(), &QQuickItemGrabResult::ready);
        if (ready.isEmpty() && !ready.wait(FRAME_TIMEOUT_MS)) {
            qFatal("the grab did not become ready");
        }
        return result->image();
    }

    QColor PixelAt(const QImage& image, qreal x, qreal y) {
        const qreal ratio = image.devicePixelRatio();
        return image.pixelColor(static_cast<int>(x * ratio),
                                static_cast<int>(y * ratio));
    }

    // The pixel in the middle of the strip to the right of the grid, at the
    // row; and in the middle of the strip below it, at the column.
    QColor Right(const QImage& image, int row) {
        return PixelAt(image,
                       MARGIN + COLUMNS * Cell_.Width + EXTRA_WIDTH / 2.0,
                       MARGIN + (row + 0.5) * Cell_.Height);
    }
    QColor Below(const QImage& image, int column) {
        return PixelAt(image, MARGIN + (column + 0.5) * Cell_.Width,
                       MARGIN + ROWS * Cell_.Height + EXTRA_HEIGHT / 2.0);
    }
    QColor Corner(const QImage& image) {
        return PixelAt(image,
                       MARGIN + COLUMNS * Cell_.Width + EXTRA_WIDTH / 2.0,
                       MARGIN + ROWS * Cell_.Height + EXTRA_HEIGHT / 2.0);
    }

    void Snapshot(const QImage& image, const QString& step) {
        const QString directory = qEnvironmentVariable("ROOST_TEST_SNAPSHOTS");
        if (directory.isEmpty()) {
            return;
        }
        QVERIFY(QDir().mkpath(directory));
        // The window colour under the picture, as the user sees it; the
        // bottom right corner of the grid, 8 times larger.
        QImage picture(image.size(), QImage::Format_ARGB32);
        picture.fill(Window_.color());
        QPainter painter(&picture);
        painter.drawImage(0, 0, image);
        painter.end();
        const int side = 24;
        const QRect corner(
            static_cast<int>(MARGIN + COLUMNS * Cell_.Width) - side / 2,
            static_cast<int>(MARGIN + ROWS * Cell_.Height) - side / 2,
            side + EXTRA_WIDTH, side + EXTRA_HEIGHT);
        QVERIFY(
            picture.copy(corner)
                .scaled(corner.size() * 8, Qt::KeepAspectRatio,
                        Qt::FastTransformation)
                .save(QDir(directory).filePath(
                    QStringLiteral("%1-%2.png")
                        .arg(QString::fromLatin1(QTest::currentTestFunction()),
                             step))));
    }

    static constexpr int MARGIN = 10;

private slots:
    void init() {
        // A colour that the item never paints, so that a hole shows.
        Window_.setColor(Qt::magenta);
    }

    void cleanup() {
        Window_.hide();
        delete Item_;
        Item_ = nullptr;
        Session_.reset();
    }

    // The strips below and to the right of the grid, the corner between them
    // too, have the default background: here a dark one, as a theme gives it.
    void TheRemainderAroundTheGridHasTheDefaultBackground() {
        auto session = NewSession();
        SetBackground(*session, DARK);
        Show(session);
        const auto background = Session_->RenderState().DefaultBackground();
        QCOMPARE(QColor(background.R, background.G, background.B).rgba(),
                 DARK.rgba());

        const QImage image = Grab();
        Snapshot(image, QStringLiteral("remainder"));
        QCOMPARE(Right(image, 0).rgba(), DARK.rgba());
        QCOMPARE(Right(image, ROWS - 1).rgba(), DARK.rgba());
        QCOMPARE(Below(image, 0).rgba(), DARK.rgba());
        QCOMPARE(Below(image, COLUMNS - 1).rgba(), DARK.rgba());
        QCOMPARE(Corner(image).rgba(), DARK.rgba());
        // A cell of the grid, next to the strip, is the same.
        QCOMPARE(PixelAt(image, MARGIN + 0.5 * Cell_.Width,
                         MARGIN + ROWS * Cell_.Height - 1.0)
                     .rgba(),
                 DARK.rgba());
        // Outside the item the background does not reach.
        QVERIFY(PixelAt(image, 2, 2) != DARK);
    }

    // The remainder is the background the terminal has now, and the item as it
    // is now: another default background and a larger item.
    void TheRemainderFollowsTheBackgroundAndTheSizeOfTheItem() {
        auto session = NewSession();
        SetBackground(*session, DARK);
        Show(session);

        SetBackground(*Session_, GREEN);
        QVERIFY(DrawFrame());
        QImage image = Grab();
        QCOMPARE(Right(image, 3).rgba(), GREEN.rgba());
        QCOMPARE(Corner(image).rgba(), GREEN.rgba());

        // One more cell of width fits, and a strip of the same width is left.
        const qreal width = Item_->width();
        Item_->setWidth(width + Cell_.Width);
        Window_.resize(Window_.width() + Cell_.Width, Window_.height());
        QVERIFY(DrawFrame());
        image = Grab();
        QCOMPARE(
            PixelAt(image,
                    MARGIN + (COLUMNS + 1) * Cell_.Width + EXTRA_WIDTH / 2.0,
                    MARGIN + 0.5 * Cell_.Height)
                .rgba(),
            GREEN.rgba());
    }

private:
    QQuickWindow Window_;
    TerminalItem* Item_ = nullptr;
    std::shared_ptr<TerminalSession> Session_;
    roost::CellMetrics Cell_;
};

QTEST_MAIN(TerminalItemRemainderTest)
#include "terminal_item_remainder_test.moc"
