// What shows at the edges of the window, against the light design
// (18-style.md): the gap between two cards is the canvas, and the row of the
// menu bar takes no room where Qt makes the menu bar native. In the real window
// against a real roostd. With ROOST_TEST_SNAPSHOTS set to a directory, the
// window is saved there as a picture.

#include <QDir>
#include <QTest>
#include <optional>

#include "core/theme_source.hpp"
#include "helpers/layout_window.hpp"
#include "helpers/scoped_env.hpp"

using namespace roost;
using namespace roost::testing;

class WindowEdgesTest : public LayoutWindow {
    Q_OBJECT

private:
    std::optional<ScopedEnv> Home_;
    std::optional<ScopedEnv> Socket_;
    std::optional<ScopedEnv> Config_;

    // The window as the user sees it, with the colour of the window under the
    // items: grabWindow. The content item's picture (grabToImage) has no
    // clear colour: where no item paints it is black. grabWindow distorts the
    // terminal offscreen, but the pixels read here (the gap, the margin, a card
    // without a terminal) are plain colours.
    QImage Picture() {
        QImage picture = Window()->grabWindow();
        if (picture.isNull()) {
            qFatal("grabWindow gave no image");
        }
        return picture.convertToFormat(QImage::Format_ARGB32);
    }

    // The colour of a token of the default theme.
    static QColor Token(const char* name) {
        return QColor(ResolveTheme({}).Tokens.value(name).toString());
    }

    QQuickItem* Child(const char* name) {
        return Root()->findChild<QQuickItem*>(name);
    }

    // The window is up with the default layout, a list and a terminal.
    void BootPair() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
    }

private slots:
    void init() {
        Prepare();
        QVERIFY(QDir().mkpath(Dir_->filePath("home")));
        Home_.emplace("HOME", Dir_->filePath("home").toUtf8());
        Socket_.emplace("ROOST_SOCKET", SocketPath().toUtf8());
        Config_.emplace("ROOST_CONFIG", Dir_->filePath("config").toUtf8());
    }

    void cleanup() {
        Teardown();
        Config_.reset();
        Socket_.reset();
        Home_.reset();
    }

    // The handle of the split lies in the gap between the cards and draws
    // nothing: the gap, and the margin around the cards, show the canvas. A
    // card shows its own colour at the same height. (#266: the gap showed the
    // white of the old window background; by the light design it is the
    // canvas, so this is a guard, not a fix.)
    void TheGapBetweenTheCardsShowsTheCanvas() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        QQuickItem* handle = Child("splitHandle");
        QVERIFY(handle != nullptr);
        QVERIFY(handle->width() > 0);
        QQuickItem* list = PanelByInstance("list");
        QVERIFY(list != nullptr);

        const QImage picture = Picture();
        const QColor canvas = Token("canvas");
        const QColor surface = Token("surface");
        QVERIFY(canvas != surface);
        const QPointF gap = handle->mapToScene(
            QPointF(handle->width() / 2, handle->height() / 2));
        QCOMPARE(picture.pixelColor(gap.toPoint()).rgba(), canvas.rgba());
        // The margin under the cards.
        QCOMPARE(picture.pixelColor(picture.width() / 2, picture.height() - 4)
                     .rgba(),
                 canvas.rgba());
        // A pixel inside the card of the list, away from its text: a corner
        // of the card, past the outline and the rounded arc.
        const QPointF inside = list->mapToScene(QPointF(8, list->height() - 8));
        QCOMPARE(picture.pixelColor(inside.toPoint()).rgba(), surface.rgba());

        const QString directory = qEnvironmentVariable("ROOST_TEST_SNAPSHOTS");
        if (!directory.isEmpty()) {
            QVERIFY(QDir().mkpath(directory));
            QVERIFY(picture.save(QDir(directory).filePath("gap-canvas.png")));
        }
    }

    // macOS: a MenuBar that Qt makes native has the height 0
    // (QQuickMenuBarPrivate::createNativeMenuBar), and the row of the layout
    // is 0 then: the panels start right under the title bar. The native bar
    // cannot be had offscreen (no QPlatformMenuBar), so the case sets the
    // height as Qt does and asks the loader to decide again. Elsewhere there
    // is no menu bar, and the layout starts right under the toolbar.
    void TheMenuBarRowTakesNoRoomWhereTheMenuIsNative() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        QQuickItem* loader = Child("menuBarLoader");
        QQuickItem* layoutView = Child("layoutView");
        QQuickItem* toolBar = Child("toolBar");
        QVERIFY(loader != nullptr);
        QVERIFY(layoutView != nullptr);
        QVERIFY(toolBar != nullptr);
#ifdef Q_OS_MACOS
        QVERIFY(loader->isVisible());
        auto* bar = loader->property("item").value<QQuickItem*>();
        QVERIFY(bar != nullptr);
        // Offscreen the bar is in the window, above the panels: not native.
        QVERIFY(!loader->property("nativeBar").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(loader->height() > 0, TIMEOUT_MS);
        const qreal row = loader->height();
        const qreal top = layoutView->y();
        QCOMPARE(row, bar->implicitHeight());

        bar->setHeight(0);
        QVERIFY(QMetaObject::invokeMethod(loader, "detectNative"));
        QVERIFY(loader->property("nativeBar").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(loader->height(), 0.0, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(layoutView->y(), top - row, TIMEOUT_MS);
#else
        QVERIFY(!loader->isVisible());
        QCOMPARE(loader->height(), 0.0);
        // No row takes room above the toolbar: it is the first item of the
        // window, and the layout lies under it at the margin of the window,
        // the one it has to the left (the banner is hidden when connected).
        QCOMPARE(toolBar->y(), 0.0);
        QVERIFY(layoutView->x() > 0);
        QCOMPARE(layoutView->y(),
                 toolBar->y() + toolBar->height() + layoutView->x());
#endif
    }

    // The row of an in-window bar follows the bar: a larger interface font
    // makes the bar taller, and the row grows with it (it once kept the first
    // height of the item). Only macOS has the loader.
    void TheMenuBarRowFollowsTheHeightOfTheBar() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
#ifdef Q_OS_MACOS
        QQuickItem* loader = Child("menuBarLoader");
        QQuickItem* layoutView = Child("layoutView");
        QVERIFY(loader != nullptr);
        QVERIFY(layoutView != nullptr);
        auto* bar = loader->property("item").value<QQuickItem*>();
        QVERIFY(bar != nullptr);
        QVERIFY(!loader->property("nativeBar").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(loader->height() > 0, TIMEOUT_MS);
        const qreal row = loader->height();
        const qreal top = layoutView->y();

        QFont font = bar->property("font").value<QFont>();
        font.setPixelSize(font.pixelSize() > 0 ? font.pixelSize() * 3 : 48);
        bar->setProperty("font", font);
        QTRY_VERIFY_WITH_TIMEOUT(bar->implicitHeight() > row, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(loader->height(), bar->implicitHeight(),
                                  TIMEOUT_MS);
        QCOMPARE(bar->height(), bar->implicitHeight());
        QTRY_COMPARE_WITH_TIMEOUT(layoutView->y(), top + loader->height() - row,
                                  TIMEOUT_MS);
#else
        QSKIP("The menu bar row is only on macOS");
#endif
    }
};

QTEST_MAIN(WindowEdgesTest)
#include "window_edges_test.moc"
