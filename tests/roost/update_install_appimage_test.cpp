// The installer of the AppImage (design/17-updates.md), Linux: the real files
// in temporary directories, the real process for the relaunch. Never the
// user's $APPIMAGE or $HOME: both are set to the temporary directory here.
// The target is read from APPIMAGE and APPDIR as the window reads them, the
// package replaces the fake AppImage with the same bytes and a mode that can
// be run, a failure leaves it as it was, and the new version is started
// without the variables of the old mount and in the directory the old one was
// started from.

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

#include <algorithm>
#include <optional>

#include "helpers/scoped_env.hpp"
#include "qml_api/update_install.hpp"

using namespace roost;
using namespace roost::testing;

namespace {

const QByteArray OLD_CONTENT = "old appimage\n";
const QByteArray NEW_CONTENT = QByteArray(100000, 'n') + "new appimage\n";

QByteArray Digest(const QByteArray& content) {
    return QCryptographicHash::hash(content, QCryptographicHash::Sha256)
        .toHex();
}

void Write(const QString& path, const QByteArray& content,
           QFileDevice::Permissions permissions) {
    QSaveFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(content);
    QVERIFY(file.commit());
    QVERIFY(QFile::setPermissions(path, permissions));
}

QByteArray Read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

constexpr QFileDevice::Permissions EXECUTABLE =
    QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;

class UpdateInstallAppImageTest : public QObject {
    Q_OBJECT

private slots:
    void init() {
        QVERIFY(Dir_.isValid());
        AppImage_ = Dir_.filePath("Apps/roost.AppImage");
        Package_ = Dir_.filePath("cache/roost-x86_64.AppImage");
        QVERIFY(QDir().mkpath(Dir_.filePath("Apps")));
        QVERIFY(QDir().mkpath(Dir_.filePath("cache")));
        // The logs of the relaunch go beside the socket: the one of this
        // directory, never the user's.
        QVERIFY(QDir().mkpath(Dir_.filePath("run")));
        Socket_.emplace("ROOST_SOCKET",
                        Dir_.filePath("run/roostd.sock").toLocal8Bit());
        Write(AppImage_, OLD_CONTENT, EXECUTABLE);
        Write(Package_, NEW_CONTENT, QFile::ReadOwner | QFile::WriteOwner);
    }

    void cleanup() {
        Socket_.reset();
        QDir(Dir_.path()).removeRecursively();
    }

    // The target is the AppImage this run is in: APPIMAGE, a file, with the
    // executable under APPDIR. A build tree, an extracted AppDir (no
    // APPIMAGE) and the environment of another app give none.
    void TheTargetIsTheRunningAppImage() {
        const ScopedEnv appImage("APPIMAGE", AppImage_.toLocal8Bit());
        const ScopedEnv appDir(
            "APPDIR", QCoreApplication::applicationDirPath().toLocal8Bit());
        QCOMPARE(installTarget(), AppImage_);
    }

    void ThereIsNoTargetOutsideAnAppImage() {
        const ScopedEnv appDir(
            "APPDIR", QCoreApplication::applicationDirPath().toLocal8Bit());
        {
            const ScopedEnv appImage("APPIMAGE", std::nullopt);
            QCOMPARE(installTarget(), QString());
        }
        {
            // A file that is not there.
            const ScopedEnv appImage("APPIMAGE",
                                     Dir_.filePath("Apps/gone").toLocal8Bit());
            QCOMPARE(installTarget(), QString());
        }
        {
            // The environment of another app: this executable is elsewhere.
            const ScopedEnv appImage("APPIMAGE", AppImage_.toLocal8Bit());
            const ScopedEnv other("APPDIR", "/opt/other-app");
            QCOMPARE(installTarget(), QString());
        }
    }

    void InstallingReplacesTheFileAndKeepsNothing() {
        const auto done = installPackage({.Package = Package_,
                                          .Digest = Digest(NEW_CONTENT),
                                          .Version = "9.9.9",
                                          .Target = AppImage_,
                                          .Rename = {},
                                          .Trash = {}});
        QVERIFY2(done.has_value(), qPrintable(done ? QString() : done.error()));
        QVERIFY(done->Old.isEmpty());
        QCOMPARE(Read(AppImage_), NEW_CONTENT);
        QVERIFY(QFileInfo(AppImage_).isExecutable());
        // Nothing is left beside it: no old copy, no temporary file.
        QCOMPARE(QDir(Dir_.filePath("Apps")).entryList(QDir::Files),
                 QStringList{"roost.AppImage"});
    }

    // The package that is not the signed line is not installed.
    void AWrongDigestChangesNothing() {
        const auto done = installPackage({.Package = Package_,
                                          .Digest = Digest("something else"),
                                          .Version = "9.9.9",
                                          .Target = AppImage_,
                                          .Rename = {},
                                          .Trash = {}});
        QVERIFY(!done.has_value());
        QVERIFY(done.error().contains("SHA256SUMS"));
        QCOMPARE(Read(AppImage_), OLD_CONTENT);
    }

    // Every variable that points into the old mount is gone from the new
    // process, what is the session's stays, and it starts where the old one
    // was started from.
    void TheRelaunchStartsTheTargetWithAFreshEnvironment() {
        const QString owd = Dir_.filePath("project");
        QVERIFY(QDir().mkpath(owd));
        const QString out = Dir_.filePath("out.txt");
        WriteFakeAppImage(out);
        const ScopedEnv home("HOME", Dir_.path().toLocal8Bit());
        const ScopedEnv appDir("APPDIR", "/tmp/.mount_roostAbc");
        const ScopedEnv appImage("APPIMAGE", AppImage_.toLocal8Bit());
        const ScopedEnv argv0("ARGV0", "roost.AppImage");
        const ScopedEnv owdVariable("OWD", owd.toLocal8Bit());
        const ScopedEnv plugins("QT_PLUGIN_PATH", "/tmp/.mount_roostAbc/p");
        const ScopedEnv imports("QML2_IMPORT_PATH", "/tmp/.mount_roostAbc/q");
        const ScopedEnv libraries("LD_LIBRARY_PATH", "/tmp/.mount_roostAbc/l");
        const ScopedEnv kept("ROOST_TEST_KEPT", "yes");
        const ScopedEnv where("ROOST_TEST_OUT", out.toLocal8Bit());

        const auto started = relaunch(AppImage_);
        QVERIFY2(started.has_value(),
                 qPrintable(started ? QString() : started.error()));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(out), 30000);
        const QList<QByteArray> lines = Read(out).split('\n');
        QCOMPARE(lines.first(),
                 QFileInfo(owd).canonicalFilePath().toLocal8Bit());
        for (const char* name :
             {"APPDIR", "APPIMAGE", "ARGV0", "OWD", "QT_PLUGIN_PATH",
              "QML2_IMPORT_PATH", "LD_LIBRARY_PATH"}) {
            QVERIFY2(!HasVariable(lines, name), name);
        }
        QVERIFY(HasVariable(lines, "ROOST_TEST_KEPT"));
        QVERIFY(HasVariable(lines, "PATH"));
    }

    void WithoutOwdTheRelaunchStartsAtHome() {
        const QString out = Dir_.filePath("out.txt");
        WriteFakeAppImage(out);
        const ScopedEnv home("HOME", Dir_.path().toLocal8Bit());
        const ScopedEnv owdVariable("OWD", std::nullopt);
        const ScopedEnv where("ROOST_TEST_OUT", out.toLocal8Bit());
        const auto started = relaunch(AppImage_);
        QVERIFY2(started.has_value(),
                 qPrintable(started ? QString() : started.error()));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(out), 30000);
        QCOMPARE(Read(out).split('\n').first(),
                 QFileInfo(Dir_.path()).canonicalFilePath().toLocal8Bit());
    }

    // What the new one prints before it has a log of its own is kept, as for
    // roostd, and is the log of this start only.
    void TheOutputOfTheNewOneIsKeptInTheRelaunchLog() {
        const QString out = Dir_.filePath("out.txt");
        const QString log = Dir_.filePath("run/roost.relaunch.log");
        Write(log, "left by an earlier start\n",
              QFile::ReadOwner | QFile::WriteOwner);
        WriteFakeAppImage(out);
        const ScopedEnv home("HOME", Dir_.path().toLocal8Bit());
        const ScopedEnv where("ROOST_TEST_OUT", out.toLocal8Bit());
        const auto started = relaunch(AppImage_);
        QVERIFY2(started.has_value(),
                 qPrintable(started ? QString() : started.error()));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(out), 30000);
        QCOMPARE(Read(log), QByteArray("to stdout\nto stderr\n"));
    }

    // No FUSE: the window runs from an extracted AppImage (its APPDIR is a
    // plain directory), and the new one is started so too. Inside a mount it
    // is not, and the user's own value stays.
    void AnExtractedWindowStartsTheNewOneExtracted() {
        const QString extracted = Dir_.filePath("squashfs-root");
        QVERIFY(QDir().mkpath(extracted));
        const ScopedEnv appDir("APPDIR", extracted.toLocal8Bit());
        const ScopedEnv user("APPIMAGE_EXTRACT_AND_RUN", std::nullopt);
        QHash<QString, QByteArray> variables;
        StartAndRead(variables);
        QCOMPARE(variables.value("APPIMAGE_EXTRACT_AND_RUN"), QByteArray("1"));
    }

    void AMountedWindowStartsTheNewOneMounted() {
        const ScopedEnv appDir("APPDIR", "/dev");
        const ScopedEnv user("APPIMAGE_EXTRACT_AND_RUN", std::nullopt);
        QHash<QString, QByteArray> variables;
        StartAndRead(variables);
        QVERIFY(!variables.contains("APPIMAGE_EXTRACT_AND_RUN"));
    }

    void TheUsersExtractAndRunReachesTheNewOne() {
        const QString extracted = Dir_.filePath("squashfs-root");
        QVERIFY(QDir().mkpath(extracted));
        const ScopedEnv appDir("APPDIR", extracted.toLocal8Bit());
        const ScopedEnv user("APPIMAGE_EXTRACT_AND_RUN", "0");
        QHash<QString, QByteArray> variables;
        StartAndRead(variables);
        QCOMPARE(variables.value("APPIMAGE_EXTRACT_AND_RUN"), QByteArray("0"));
    }

    void ATargetThatCannotBeStartedIsAFailureWithItsPath() {
        const QString missing = Dir_.filePath("Apps/missing.AppImage");
        const auto started = relaunch(missing);
        QVERIFY(!started.has_value());
        QVERIFY(started.error().contains(missing));
    }

private:
    // The variables the new version is started with, by name.
    void StartAndRead(QHash<QString, QByteArray>& variables) {
        const QString out = Dir_.filePath("out.txt");
        WriteFakeAppImage(out);
        const ScopedEnv home("HOME", Dir_.path().toLocal8Bit());
        const ScopedEnv where("ROOST_TEST_OUT", out.toLocal8Bit());
        const auto started = relaunch(AppImage_);
        QVERIFY2(started.has_value(),
                 qPrintable(started ? QString() : started.error()));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(out), 30000);
        for (const QByteArray& line : Read(out).split('\n')) {
            const qsizetype equals = line.indexOf('=');
            if (equals > 0) {
                variables.insert(QString::fromLocal8Bit(line.first(equals)),
                                 line.sliced(equals + 1));
            }
        }
    }

    // The new version: writes its directory and its environment to the file,
    // atomically (the test waits for the file).
    void WriteFakeAppImage(const QString& out) {
        Write(AppImage_,
              "#!/bin/sh\n"
              "echo 'to stdout'; echo 'to stderr' >&2\n"
              "{ pwd -P; env; } > \"$ROOST_TEST_OUT.tmp\"\n"
              "mv \"$ROOST_TEST_OUT.tmp\" \"$ROOST_TEST_OUT\"\n",
              EXECUTABLE);
        QVERIFY(!QFileInfo::exists(out));
    }

    static bool HasVariable(const QList<QByteArray>& lines, const char* name) {
        const QByteArray prefix = QByteArray(name) + "=";
        return std::any_of(
            lines.begin(), lines.end(),
            [&](const QByteArray& line) { return line.startsWith(prefix); });
    }

    QTemporaryDir Dir_;
    std::optional<ScopedEnv> Socket_;
    QString AppImage_;
    QString Package_;
};

}  // namespace

QTEST_GUILESS_MAIN(UpdateInstallAppImageTest)
#include "update_install_appimage_test.moc"
