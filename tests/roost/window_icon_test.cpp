// The icon of the window: PNGs of 16 to 512 pixels are compiled in, every one
// a real picture of its size, and InstallWindowIcon puts them on the
// application, which is what gives an X11 window its _NET_WM_ICON.

#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QSize>
#include <QTest>

#include "roost/window_icon.hpp"

using namespace roost;

class WindowIconTest : public QObject {
    Q_OBJECT

private slots:
    void HasEverySizeOfThePackage() {
        const QIcon icon = WindowIcon();
        QVERIFY(!icon.isNull());
        const QList<QSize> sizes = icon.availableSizes();
        for (const int size : {16, 32, 48, 64, 128, 256, 512}) {
            QVERIFY2(sizes.contains(QSize(size, size)),
                     qPrintable(QString::number(size)));
        }
    }

    void EveryPixmapHasItsOwnSizeAndPixels_data() {
        QTest::addColumn<int>("size");
        for (const int size : {16, 32, 48, 64, 128, 256, 512}) {
            QTest::newRow(qPrintable(QString::number(size))) << size;
        }
    }
    void EveryPixmapHasItsOwnSizeAndPixels() {
        QFETCH(int, size);
        const QImage image = WindowIcon().pixmap(QSize(size, size)).toImage();
        QCOMPARE(image.size(), QSize(size, size));
        // A picture, not an empty file: more than one colour.
        QVERIFY(image.pixel(0, 0) != image.pixel(size / 2, size / 2) ||
                image.pixel(0, 0) != image.pixel(size - 1, size / 3));
    }

    void InstallSetsTheWindowIconOfTheApplication() {
        InstallWindowIcon();
        const QList<QSize> sizes =
            QGuiApplication::windowIcon().availableSizes();
        for (const int size : {16, 32, 48, 64, 128, 256}) {
            QVERIFY2(sizes.contains(QSize(size, size)),
                     qPrintable(QString::number(size)));
        }
    }
};

QTEST_MAIN(WindowIconTest)
#include "window_icon_test.moc"
