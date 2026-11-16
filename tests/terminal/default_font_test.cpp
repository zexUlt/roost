// The terminal's default font must be a real fixed-pitch face, also under
// QT_QPA_PLATFORM=offscreen where the system's fixed font can be proportional.

#include "terminal/renderer.hpp"

#include <QFontInfo>
#include <QFontMetricsF>
#include <QTest>

class DefaultFontTest : public QObject {
    Q_OBJECT

private slots:
    void ResolvesToFixedPitchFace() {
        const auto font = roost::DefaultTerminalFont();
        QVERIFY2(QFontInfo(font).fixedPitch(),
                 qPrintable(QFontInfo(font).family()));
        const QFontMetricsF metrics(font);
        const auto reference = metrics.horizontalAdvance(u'M');
        QVERIFY(reference > 0);
        for (const auto ch : {u'i', u'W', u'0'}) {
            QCOMPARE(metrics.horizontalAdvance(ch), reference);
        }
    }
};

QTEST_MAIN(DefaultFontTest)
#include "default_font_test.moc"
