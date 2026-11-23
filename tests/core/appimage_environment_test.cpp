// The environment of what an AppImage starts (design/17-updates.md,
// "Установка и перезапуск"): each family of variables that AppRun and the
// runtime set is dropped, the user's own variables stay. Pure functions: the
// input environment is written here, nothing is read from the process.

#include <QDir>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

#include "core/appimage_environment.hpp"

using namespace roost;

namespace {

// What the runtime, AppRun and the hooks of linuxdeploy put in, and what the
// user's session has.
QProcessEnvironment InsideTheMount() {
    QProcessEnvironment environment;
    environment.insert("APPDIR", "/tmp/.mount_roostAbc");
    environment.insert("APPIMAGE", "/home/u/Apps/roost.AppImage");
    environment.insert("ARGV0", "roost.AppImage");
    environment.insert("OWD", "/home/u/project");
    environment.insert("QT_PLUGIN_PATH", "/tmp/.mount_roostAbc/usr/plugins");
    environment.insert("QML2_IMPORT_PATH", "/tmp/.mount_roostAbc/usr/qml");
    environment.insert("LD_LIBRARY_PATH", "/tmp/.mount_roostAbc/usr/lib");
    environment.insert("PATH", "/usr/local/bin:/usr/bin:/bin");
    environment.insert("HOME", "/home/u");
    environment.insert("DISPLAY", ":0");
    environment.insert("XDG_RUNTIME_DIR", "/run/user/1000");
    environment.insert("SHELL", "/bin/zsh");
    return environment;
}

class AppImageEnvironmentTest : public QObject {
    Q_OBJECT

private slots:
    void TheRelaunchDropsTheVariablesOfTheBundle_data() {
        QTest::addColumn<QString>("name");
        QTest::newRow("APPDIR") << "APPDIR";
        QTest::newRow("APPIMAGE") << "APPIMAGE";
        QTest::newRow("ARGV0") << "ARGV0";
        QTest::newRow("OWD") << "OWD";
        QTest::newRow("QT_PLUGIN_PATH") << "QT_PLUGIN_PATH";
        QTest::newRow("QML2_IMPORT_PATH") << "QML2_IMPORT_PATH";
        QTest::newRow("LD_LIBRARY_PATH") << "LD_LIBRARY_PATH";
    }

    void TheRelaunchDropsTheVariablesOfTheBundle() {
        QFETCH(QString, name);
        const QProcessEnvironment input = InsideTheMount();
        QVERIFY(input.contains(name));
        QVERIFY(!RelaunchEnvironment(input).contains(name));
    }

    // Everything the session has is the new version's, as from the desktop:
    // PATH as it is (AppRun does not change it), the display, the home.
    void TheRelaunchKeepsTheRest() {
        const QProcessEnvironment output =
            RelaunchEnvironment(InsideTheMount());
        QCOMPARE(output.value("PATH"), QString("/usr/local/bin:/usr/bin:/bin"));
        QCOMPARE(output.value("HOME"), QString("/home/u"));
        QCOMPARE(output.value("DISPLAY"), QString(":0"));
        QCOMPARE(output.value("XDG_RUNTIME_DIR"), QString("/run/user/1000"));
        QCOMPARE(output.value("SHELL"), QString("/bin/zsh"));
        QCOMPARE(output.keys().size(), 5);
    }

    // The same as LoginEnvironment does for what it starts: the library
    // variables go only inside an AppImage, the variables of the bundle
    // always.
    void OutsideAnAppImageTheLibraryVariablesStay() {
        const QProcessEnvironment output =
            StripAppImageEnvironment(InsideTheMount(), false);
        QVERIFY(!output.contains("APPDIR"));
        QVERIFY(!output.contains("OWD"));
        QCOMPARE(output.value("LD_LIBRARY_PATH"),
                 QString("/tmp/.mount_roostAbc/usr/lib"));
        QVERIFY(output.contains("QT_PLUGIN_PATH"));
        QVERIFY(output.contains("QML2_IMPORT_PATH"));
        const QProcessEnvironment inside =
            StripAppImageEnvironment(InsideTheMount(), true);
        QVERIFY(!inside.contains("LD_LIBRARY_PATH"));
    }

    void AnEmptyEnvironmentStaysEmpty() {
        QVERIFY(RelaunchEnvironment(QProcessEnvironment()).isEmpty());
    }

    void TheDirectoryIsOwdOrTheHome() {
        QTemporaryDir owd;
        QVERIFY(owd.isValid());
        QProcessEnvironment environment;
        QCOMPARE(RelaunchDirectory(environment, "/home/u"), QString("/home/u"));
        environment.insert("OWD", owd.path());
        QCOMPARE(RelaunchDirectory(environment, "/home/u"), owd.path());
        // Gone since: the new version starts at home, not nowhere.
        environment.insert("OWD", owd.path() + "/gone");
        QCOMPARE(RelaunchDirectory(environment, "/home/u"), QString("/home/u"));
        environment.insert("OWD", "");
        QCOMPARE(RelaunchDirectory(environment, "/home/u"), QString("/home/u"));
    }

    void InsideAnAppImageNeedsTheExecutableUnderAppDir() {
        QVERIFY(InsideAppImage("/tmp/.mount_x", "/tmp/.mount_x/usr/bin"));
        QVERIFY(InsideAppImage("/tmp/.mount_x/", "/tmp/.mount_x"));
        QVERIFY(!InsideAppImage("", "/tmp/.mount_x/usr/bin"));
        QVERIFY(!InsideAppImage("/tmp/.mount_x", "/tmp/.mount_xy/usr/bin"));
        QVERIFY(!InsideAppImage("/tmp/.mount_x", "/opt/roost/bin"));
    }
};

}  // namespace

QTEST_GUILESS_MAIN(AppImageEnvironmentTest)
#include "appimage_environment_test.moc"
