// The installer of macOS (design/17-updates.md): the swap of the running
// bundle for the one in a .dmg, on fixture bundles in a temporary directory;
// never the real /Applications. The images are made with the real hdiutil, the
// copy and the mount are the real ones. The success case and each failure that
// must leave the old bundle in place: the package changed after the check, an
// image without roost.app, a copy of another version or one without its
// executable, a directory that is not writable, a rename that fails (before
// and in the middle of the swap).

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

#include <cstdio>
#include <memory>
#include <optional>

#include "qml_api/update_install.hpp"

using namespace roost;

namespace {

const QString OFFERED = QStringLiteral("9.9.9");

void Write(const QString& path, const QByteArray& content, bool executable) {
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(content), content.size());
    file.close();
    if (executable) {
        QVERIFY(file.setPermissions(file.permissions() | QFile::ExeOwner |
                                    QFile::ExeGroup | QFile::ExeOther));
    }
}

QByteArray Read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A bundle as the package has it, as far as the installer looks: Info.plist
// with a version, the two executables, and a file that says whose it is.
struct BundleSpec {
    QString Version;
    QByteArray Marker;
    bool Daemon = true;
};

void MakeBundle(const QString& path, const BundleSpec& spec) {
    Write(path + "/Contents/Info.plist",
          "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
          "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
          "<plist version=\"1.0\"><dict>\n"
          "<key>CFBundleExecutable</key><string>roost</string>\n"
          "<key>CFBundleShortVersionString</key><string>" +
              spec.Version.toUtf8() +
              "</string>\n"
              "</dict></plist>\n",
          false);
    Write(path + "/Contents/MacOS/roost", "#!/bin/sh\nexit 0\n", true);
    if (spec.Daemon) {
        Write(path + "/Contents/MacOS/roostd", "#!/bin/sh\nexit 0\n", true);
    }
    Write(path + "/Contents/Resources/marker.txt", spec.Marker, false);
}

// A .dmg of `folder`, made like tools/package_macos.sh makes its.
void MakeImage(const QString& image, const QString& folder) {
    QProcess hdiutil;
    hdiutil.setProcessChannelMode(QProcess::MergedChannels);
    hdiutil.start("/usr/bin/hdiutil",
                  {"create", "-quiet", "-volname", "roost", "-srcfolder",
                   folder, "-fs", "HFS+", "-format", "UDZO", "-ov", image});
    QVERIFY(hdiutil.waitForFinished(120000));
    QVERIFY2(hdiutil.exitCode() == 0, hdiutil.readAll().constData());
}

// The system's rename, which moves a directory.
QString Rename(const QString& from, const QString& to) {
    return std::rename(from.toLocal8Bit().constData(),
                       to.toLocal8Bit().constData()) == 0
               ? QString()
               : QStringLiteral("rename failed");
}

QByteArray Digest(const QString& path) {
    return QCryptographicHash::hash(Read(path), QCryptographicHash::Sha256)
        .toHex();
}

// Whether a volume is mounted at a path under `directory`.
bool MountedUnder(const QString& directory) {
    QProcess mount;
    mount.start("/sbin/mount", {});
    mount.waitForFinished(30000);
    return QString::fromUtf8(mount.readAllStandardOutput())
        .contains(QFileInfo(directory).canonicalFilePath());
}

}  // namespace

class UpdateInstallTest : public QObject {
    Q_OBJECT

private:
    // The image of a folder that has roost.app made by `spec`, or, without
    // `app`, other files only.
    QString Image(const QString& name, const std::optional<BundleSpec>& app) {
        const QString folder = Images_->filePath(name + "-folder");
        if (app) {
            MakeBundle(folder + "/roost.app", *app);
        } else {
            Write(folder + "/README.txt", "not an app", false);
        }
        const QString image = Images_->filePath(name + ".dmg");
        MakeImage(image, folder);
        return image;
    }

    QString Target() const { return Dir_->filePath("Applications/roost.app"); }
    QString Applications() const { return Dir_->filePath("Applications"); }

    // The names in the directory of the bundle: nothing but roost.app is left
    // by a failure.
    QStringList Apps() const {
        return QDir(Applications())
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    }

    // The request for `image` as a case copies it into the cache of its own.
    InstallRequest Request(const QString& image) {
        const QString package =
            Dir_->filePath("cache/" + QFileInfo(image).fileName());
        QDir().mkpath(Dir_->filePath("cache"));
        QFile::copy(image, package);
        return {.Package = package,
                .Digest = Digest(package),
                .Version = OFFERED,
                .Target = Target(),
                .Rename = {},
                .Trash = [this](const QString& path) { return ToTrash(path); }};
    }

    // The Trash of the case, not the user's.
    QString Trash() const { return Dir_->filePath("Trash"); }
    QString ToTrash(const QString& path) const {
        const QString to = Trash() + "/" + QFileInfo(path).fileName();
        return QDir().mkpath(Trash()) && QDir().rename(path, to) ? to
                                                                 : QString();
    }

    void CheckUnchanged() {
        QCOMPARE(Apps(), QStringList{"roost.app"});
        QCOMPARE(Read(Target() + "/Contents/Resources/marker.txt"),
                 QByteArray("old"));
        // The image is detached and its mount point is gone.
        QVERIFY(!MountedUnder(Dir_->path()));
        QVERIFY(QDir(Dir_->filePath("cache"))
                    .entryList({"mount-*"}, QDir::Dirs)
                    .isEmpty());
    }

    std::unique_ptr<QTemporaryDir> Images_;
    std::unique_ptr<QTemporaryDir> Dir_;

private slots:
    void initTestCase() {
#ifndef Q_OS_MACOS
        QSKIP("macOS only");
#endif
        Images_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Images_->isValid());
        QVERIFY(!Image("good", BundleSpec{OFFERED, "new"}).isEmpty());
        // A good image of the offered version, but not the one that was
        // checked: what a swap of the file in the cache would put there.
        QVERIFY(!Image("swapped", BundleSpec{OFFERED, "evil"}).isEmpty());
        QVERIFY(!Image("old-version", BundleSpec{"1.2.3", "new"}).isEmpty());
        QVERIFY(
            !Image("no-daemon", BundleSpec{OFFERED, "new", false}).isEmpty());
        QVERIFY(!Image("no-app", std::nullopt).isEmpty());
    }

    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
        MakeBundle(Target(), BundleSpec{"0.0.1", "old"});
    }

    void cleanup() {
        // A case may have made the directory read-only.
        QFile::setPermissions(Applications(), QFile::ReadOwner |
                                                  QFile::WriteOwner |
                                                  QFile::ExeOwner);
        Dir_.reset();
    }

    void ThisRunIsNoInstalledCopy() {
        // The test binary is in a build tree, not in a roost.app.
        QCOMPARE(installTarget(), QString());
    }

    void TheBundleIsReplaced() {
        const InstallRequest request = Request(Images_->filePath("good.dmg"));
        const auto done = installPackage(request);
        QVERIFY2(done.has_value(),
                 qUtf8Printable(done ? QString() : done.error()));
        // The new bundle is in place, whole.
        QCOMPARE(Read(Target() + "/Contents/Resources/marker.txt"),
                 QByteArray("new"));
        QVERIFY(QFileInfo(Target() + "/Contents/MacOS/roostd").isExecutable());
        // Nothing of the swap is left next to it, and the image is detached.
        QCOMPARE(Apps(), QStringList{"roost.app"});
        QVERIFY(!MountedUnder(Dir_->path()));
        QVERIFY(QDir(Dir_->filePath("cache"))
                    .entryList({"mount-*"}, QDir::Dirs)
                    .isEmpty());
        // The old bundle is in the Trash, whole, not deleted.
        QVERIFY(!done->Old.startsWith(Applications()));
        QVERIFY(done->Old.startsWith(Trash() + "/"));
        QCOMPARE(Read(done->Old + "/Contents/Resources/marker.txt"),
                 QByteArray("old"));
    }

    void APackageChangedAfterTheCheckIsNotInstalled() {
        const InstallRequest request = Request(Images_->filePath("good.dmg"));
        // The file in the cache is swapped for another image, a good one.
        QVERIFY(QFile::remove(request.Package));
        QVERIFY(QFile::copy(Images_->filePath("swapped.dmg"), request.Package));
        const auto done = installPackage(request);
        QVERIFY(!done);
        QVERIFY2(done.error().contains("does not match its line"),
                 qUtf8Printable(done.error()));
        QVERIFY2(done.error().contains(request.Package),
                 qUtf8Printable(done.error()));
        CheckUnchanged();
    }

    void AnImageWithoutTheAppIsNotInstalled() {
        const auto done =
            installPackage(Request(Images_->filePath("no-app.dmg")));
        QVERIFY(!done);
        QVERIFY2(done.error().contains("has no roost.app"),
                 qUtf8Printable(done.error()));
        CheckUnchanged();
    }

    void ACopyOfAnotherVersionIsNotInstalled() {
        const auto done =
            installPackage(Request(Images_->filePath("old-version.dmg")));
        QVERIFY(!done);
        QVERIFY2(
            done.error().contains("is version 1.2.3, not the offered 9.9.9"),
            qUtf8Printable(done.error()));
        CheckUnchanged();
    }

    void ACopyWithoutTheDaemonIsNotInstalled() {
        const auto done =
            installPackage(Request(Images_->filePath("no-daemon.dmg")));
        QVERIFY(!done);
        QVERIFY2(done.error().contains("Contents/MacOS/roostd"),
                 qUtf8Printable(done.error()));
        CheckUnchanged();
    }

    void ADirectoryThatIsNotWritableChangesNothing() {
        if (::geteuid() == 0) {
            QSKIP("root writes through directory permissions");
        }
        const InstallRequest request = Request(Images_->filePath("good.dmg"));
        QVERIFY(QFile::setPermissions(Applications(),
                                      QFile::ReadOwner | QFile::ExeOwner));
        const auto done = installPackage(request);
        QVERIFY(!done);
        // The path, and the steps to take by hand.
        QVERIFY2(done.error().contains(Applications() + " is not writable"),
                 qUtf8Printable(done.error()));
        QVERIFY2(done.error().contains("drag roost.app into " + Applications()),
                 qUtf8Printable(done.error()));
        QVERIFY(QFile::setPermissions(Applications(), QFile::ReadOwner |
                                                          QFile::WriteOwner |
                                                          QFile::ExeOwner));
        CheckUnchanged();
    }

    void ATargetThatIsNoBundleIsRefused() {
        InstallRequest request = Request(Images_->filePath("good.dmg"));
        request.Target = Applications();
        const auto done = installPackage(request);
        QVERIFY(!done);
        QVERIFY2(done.error().contains("is not an application bundle"),
                 qUtf8Printable(done.error()));
        CheckUnchanged();
    }

    void ARenameThatFailsLeavesTheOldBundle_data() {
        QTest::addColumn<int>("failFrom");
        QTest::addColumn<QString>("message");
        // The bundle cannot be moved aside: nothing has moved.
        QTest::newRow("first") << 1 << "is unchanged";
        // The new bundle cannot take its place, the old one is put back.
        QTest::newRow("second") << 2 << "the old";
    }

    void ARenameThatFailsLeavesTheOldBundle() {
        QFETCH(int, failFrom);
        QFETCH(QString, message);
        InstallRequest request = Request(Images_->filePath("good.dmg"));
        int calls = 0;
        // The renames fail from the numbered one on, one after the other,
        // except the put-back of the old bundle (the third), which works.
        request.Rename = [&](const QString& from,
                             const QString& to) -> QString {
            ++calls;
            if (calls >= failFrom && calls != 3) {
                return "forced";
            }
            return Rename(from, to);
        };
        const auto done = installPackage(request);
        QVERIFY(!done);
        QVERIFY2(
            done.error().contains("forced") && done.error().contains(message),
            qUtf8Printable(done.error()));
        CheckUnchanged();
    }

    void ARenameThatFailsAndCannotBePutBackSaysWhereTheOldBundleIs() {
        InstallRequest request = Request(Images_->filePath("good.dmg"));
        int calls = 0;
        request.Rename = [&](const QString& from,
                             const QString& to) -> QString {
            ++calls;
            if (calls >= 2) {
                return "forced";
            }
            return Rename(from, to);
        };
        const auto done = installPackage(request);
        QVERIFY(!done);
        QVERIFY2(done.error().contains("could not be put back"),
                 qUtf8Printable(done.error()));
        // The old bundle is where the message says, whole; the new one is
        // gone.
        const QStringList left =
            QDir(Applications())
                .entryList({"roost-old-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(left.size(), 1);
        const QString old = Applications() + "/" + left.first();
        QVERIFY2(done.error().contains(old), qUtf8Printable(done.error()));
        QCOMPARE(Read(old + "/Contents/Resources/marker.txt"),
                 QByteArray("old"));
        QVERIFY(QDir(Applications())
                    .entryList({"roost-new-*"}, QDir::Dirs)
                    .isEmpty());
    }

    void ABundleThatCannotBeStartedIsReported() {
        const auto started = relaunch(Dir_->filePath("nothing/roost.app"));
        QVERIFY(!started);
        QVERIFY2(started.error().contains(Dir_->filePath("nothing/roost.app")),
                 qUtf8Printable(started.error()));
    }
};

QTEST_MAIN(UpdateInstallTest)
#include "update_install_test.moc"
