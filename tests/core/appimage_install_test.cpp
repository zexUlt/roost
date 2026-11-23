// Replacing the running AppImage (design/17-updates.md): the package is copied
// beside the AppImage and renamed over it. Real files in temporary
// directories; never the $APPIMAGE of the user.

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

#include "core/appimage_install.hpp"

using namespace roost;

namespace {

const QByteArray OLD_CONTENT = "old appimage\n";
const QByteArray NEW_CONTENT = QByteArray(300000, 'n') + "new appimage\n";

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

// The text of a failure, empty for a success (QVERIFY2 evaluates it always).
QString Error(const std::expected<void, QString>& result) {
    return result ? QString() : result.error();
}

constexpr QFileDevice::Permissions EXECUTABLE =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;

class AppImageInstallTest : public QObject {
    Q_OBJECT

private slots:
    void init() {
        QVERIFY(Dir_.isValid());
        QDir(Dir_.path()).removeRecursively();
        QVERIFY(QDir().mkpath(Dir_.path() + "/bin"));
        QVERIFY(QDir().mkpath(Dir_.path() + "/cache"));
        AppImage_ = Dir_.path() + "/bin/roost.AppImage";
        Package_ = Dir_.path() + "/cache/roost-new.AppImage";
        Write(AppImage_, OLD_CONTENT, EXECUTABLE);
        Write(Package_, NEW_CONTENT,
              QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    void cleanup() {
        // A case that made a directory read-only must not block the removal.
        QFile::setPermissions(Dir_.path() + "/bin",
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner);
    }

    void ReplacesTheFile() {
        const auto result =
            ReplaceAppImage(AppImage_, Package_, Digest(NEW_CONTENT));
        QVERIFY2(result.has_value(), qPrintable(Error(result)));
        QCOMPARE(Read(AppImage_), NEW_CONTENT);
        const auto permissions = QFile::permissions(AppImage_);
        QVERIFY(permissions & QFileDevice::ExeOwner);
        QVERIFY(permissions & QFileDevice::ExeGroup);
        QVERIFY(permissions & QFileDevice::ExeOther);
        QCOMPARE(
            QDir(Dir_.path() + "/bin").entryList(QDir::Files | QDir::Hidden),
            QStringList{"roost.AppImage"});
        // The package in the cache stays: the caller removes it.
        QCOMPARE(Read(Package_), NEW_CONTENT);
    }

    void DoesNotWriteInPlace() {
        // The running process maps the old file: it must stay as it was, only
        // the name moves to a new file.
        QFile old(AppImage_);
        QVERIFY(old.open(QIODevice::ReadOnly));
        QVERIFY(ReplaceAppImage(AppImage_, Package_, Digest(NEW_CONTENT)));
        QCOMPARE(old.readAll(), OLD_CONTENT);
    }

    void KeepsTheOwnerBits() {
        QVERIFY(QFile::setPermissions(AppImage_,
                                      EXECUTABLE | QFileDevice::ReadGroup));
        QVERIFY(ReplaceAppImage(AppImage_, Package_, Digest(NEW_CONTENT)));
        QVERIFY(QFile::permissions(AppImage_) & QFileDevice::ReadGroup);
    }

    void RefusesWhenTheDigestChanged() {
        const auto result =
            ReplaceAppImage(AppImage_, Package_, Digest("something else"));
        QVERIFY(!result.has_value());
        QVERIFY2(Error(result).contains("SHA256SUMS"),
                 qPrintable(Error(result)));
        QVERIFY2(Error(result).contains(Package_), qPrintable(Error(result)));
        QCOMPARE(Read(AppImage_), OLD_CONTENT);
        QCOMPARE(
            QDir(Dir_.path() + "/bin").entryList(QDir::Files | QDir::Hidden),
            QStringList{"roost.AppImage"});
    }

    void RefusesWhenThePackageChangedInTheCache() {
        const QByteArray digest = Digest(NEW_CONTENT);
        Write(Package_, "tampered", QFileDevice::ReadOwner);
        QVERIFY(!ReplaceAppImage(AppImage_, Package_, digest));
        QCOMPARE(Read(AppImage_), OLD_CONTENT);
    }

    void FailsInAReadOnlyDirectory() {
        if (geteuid() == 0) {
            QSKIP("root writes anywhere");
        }
        QVERIFY(QFile::setPermissions(
            Dir_.path() + "/bin",
            QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const auto result =
            ReplaceAppImage(AppImage_, Package_, Digest(NEW_CONTENT));
        QVERIFY(!result.has_value());
        QVERIFY2(Error(result).contains(Dir_.path() + "/bin"),
                 qPrintable(Error(result)));
        QVERIFY2(Error(result).contains("release page"),
                 qPrintable(Error(result)));
        QCOMPARE(Read(AppImage_), OLD_CONTENT);
        QCOMPARE(
            QDir(Dir_.path() + "/bin").entryList(QDir::Files | QDir::Hidden),
            QStringList{"roost.AppImage"});
    }

    void FailsWhenThePackageIsMissing() {
        QVERIFY(QFile::remove(Package_));
        const auto result =
            ReplaceAppImage(AppImage_, Package_, Digest(NEW_CONTENT));
        QVERIFY(!result.has_value());
        QVERIFY2(Error(result).contains(Package_), qPrintable(Error(result)));
        QCOMPARE(Read(AppImage_), OLD_CONTENT);
    }

    void FailsWhenTheAppImageIsMissing() {
        QVERIFY(QFile::remove(AppImage_));
        const auto result =
            ReplaceAppImage(AppImage_, Package_, Digest(NEW_CONTENT));
        QVERIFY(!result.has_value());
        QVERIFY2(Error(result).contains(AppImage_), qPrintable(Error(result)));
        // Nothing is created in its place.
        QVERIFY(QDir(Dir_.path() + "/bin")
                    .entryList(QDir::Files | QDir::Hidden)
                    .isEmpty());
    }

    void ReplacesTheTargetOfASymlink() {
        QVERIFY(QDir().mkpath(Dir_.path() + "/links"));
        const QString link = Dir_.path() + "/links/roost";
        QVERIFY(QFile::link(AppImage_, link));
        QVERIFY(ReplaceAppImage(link, Package_, Digest(NEW_CONTENT)));
        // The link is still a link, to the file that has the new content.
        QVERIFY(QFileInfo(link).isSymLink());
        QCOMPARE(QFileInfo(link).symLinkTarget(), AppImage_);
        QCOMPARE(Read(AppImage_), NEW_CONTENT);
        QVERIFY(QFileInfo(AppImage_).isExecutable());
        QCOMPARE(QDir(Dir_.path() + "/links")
                     .entryList(QDir::Files | QDir::Hidden | QDir::System),
                 QStringList{"roost"});
        QCOMPARE(
            QDir(Dir_.path() + "/bin").entryList(QDir::Files | QDir::Hidden),
            QStringList{"roost.AppImage"});
    }

    void FailsOnADanglingSymlink() {
        const QString link = Dir_.path() + "/bin/gone";
        QVERIFY(QFile::link(Dir_.path() + "/nowhere", link));
        QVERIFY(!ReplaceAppImage(link, Package_, Digest(NEW_CONTENT)));
        QVERIFY(QFileInfo(link).isSymLink());
        QVERIFY(!QFileInfo::exists(Dir_.path() + "/nowhere"));
    }

    void DetectsTheRunningAppImage() {
        const QString appDir = Dir_.path() + "/mnt";
        QCOMPARE(AppImagePath(AppImage_, appDir, appDir + "/usr/bin"),
                 std::optional<QString>(AppImage_));
        QCOMPARE(AppImagePath(AppImage_, appDir, appDir),
                 std::optional<QString>(AppImage_));
    }

    void NoAppImageWithoutTheVariables() {
        const QString appDir = Dir_.path() + "/mnt";
        QVERIFY(!AppImagePath(QString(), appDir, appDir + "/usr/bin"));
        // An extracted AppDir has APPDIR but no APPIMAGE; a `cmake --install`
        // tree has neither.
        QVERIFY(!AppImagePath(QString(), appDir, appDir + "/usr/bin"));
        QVERIFY(!AppImagePath(AppImage_, QString(), "/usr/local/bin"));
    }

    void NoAppImageWhenThePathIsNoFile() {
        const QString appDir = Dir_.path() + "/mnt";
        QVERIFY(
            !AppImagePath(Dir_.path() + "/bin", appDir, appDir + "/usr/bin"));
        QVERIFY(!AppImagePath(Dir_.path() + "/missing", appDir,
                              appDir + "/usr/bin"));
    }

    void NoAppImageOfAnotherEnvironment() {
        // APPIMAGE and APPDIR of another app, this executable is elsewhere.
        QVERIFY(
            !AppImagePath(AppImage_, Dir_.path() + "/mnt", "/opt/roost/bin"));
        QVERIFY(!AppImagePath(AppImage_, Dir_.path() + "/mnt",
                              Dir_.path() + "/mnt-other/usr/bin"));
    }

    void ReadsTheEnvironmentOfTheProcess() {
        qunsetenv("APPIMAGE");
        qunsetenv("APPDIR");
        QVERIFY(!RunningAppImagePath());
        qputenv("APPIMAGE", AppImage_.toLocal8Bit());
        qputenv("APPDIR", QCoreApplication::applicationDirPath().toLocal8Bit());
        QCOMPARE(RunningAppImagePath(), std::optional<QString>(AppImage_));
        qputenv("APPIMAGE", (Dir_.path() + "/bin").toLocal8Bit());
        QVERIFY(!RunningAppImagePath());
        qunsetenv("APPIMAGE");
        qunsetenv("APPDIR");
    }

private:
    QTemporaryDir Dir_;
    QString AppImage_;
    QString Package_;
};

}  // namespace

QTEST_GUILESS_MAIN(AppImageInstallTest)
#include "appimage_install_test.moc"
