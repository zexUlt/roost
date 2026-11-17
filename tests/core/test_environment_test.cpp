// The test binaries set their own environment (helpers/test_environment.cpp).
// Run under ctest twice: with the variables set, and with them removed.

#include <QGuiApplication>
#include <QTest>

#include <cstdio>

class TestEnvironmentTest : public QObject {
    Q_OBJECT

private slots:
    void PlatformIsOffscreen() {
        QCOMPARE(QGuiApplication::platformName(), QString("offscreen"));
    }
    void DiskCacheIsDisabled() {
        QCOMPARE(qgetenv("QML_DISABLE_DISK_CACHE"), QByteArray("1"));
    }
};

// Not QTEST_MAIN: when the guard is broken, QGuiApplication would load the
// native platform plugin. Fail before that, so no window or Dock icon appears.
int main(int argc, char** argv) {
    if (qgetenv("QT_QPA_PLATFORM") != "offscreen") {
        std::fputs("QT_QPA_PLATFORM is not offscreen\n", stderr);
        return 1;
    }
    QGuiApplication app(argc, argv);
    TestEnvironmentTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_environment_test.moc"
