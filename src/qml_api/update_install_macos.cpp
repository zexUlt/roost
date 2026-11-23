// The installer of macOS (design/17-updates.md): the .dmg is mounted read-only
// at a directory of ours, roost.app is copied with ditto next to the running
// bundle under a temporary name, the image is detached, the copy is checked,
// and the two bundles trade places by rename. Every step before the swap
// leaves the running bundle alone; a failed swap puts it back.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QUuid>
#include <QtLogging>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "core/update_verify.hpp"
#include "qml_api/update_install.hpp"

namespace roost {

namespace {

constexpr auto HDIUTIL = "/usr/bin/hdiutil";
constexpr auto DITTO = "/usr/bin/ditto";
constexpr auto OPEN = "/usr/bin/open";
// The tool tools/package_macos.sh reads Info.plist with.
constexpr auto PLIST_BUDDY = "/usr/libexec/PlistBuddy";
constexpr auto APP_IN_IMAGE = "roost.app";
constexpr int QUICK_MS = 30'000;
constexpr int ATTACH_MS = 120'000;
constexpr int COPY_MS = 600'000;
constexpr qsizetype OUTPUT_TAIL = 400;

// What a failed tool said, short enough for a line.
QString tail(const QByteArray& output) {
    const QString text = QString::fromUtf8(output).trimmed();
    return text.size() > OUTPUT_TAIL
               ? QStringLiteral("...") + text.last(OUTPUT_TAIL)
               : text;
}

// Runs a tool to its end: its output (the error stream too) on success, why
// not otherwise. The tool's stdin is empty: a license prompt of an image is
// answered by the end of input, with a refusal.
std::expected<QByteArray, QString> run(const QString& program,
                                       const QStringList& arguments,
                                       int timeoutMs) {
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(program, arguments);
    if (!process.waitForStarted(QUICK_MS)) {
        return std::unexpected(QStringLiteral("cannot run %1: %2")
                                   .arg(program, process.errorString()));
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(QUICK_MS);
        return std::unexpected(QStringLiteral("%1 did not finish in %2 s")
                                   .arg(QFileInfo(program).fileName())
                                   .arg(timeoutMs / 1000));
    }
    const QByteArray output = process.readAll();
    if (process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        return std::unexpected(
            QStringLiteral("%1 failed: %2")
                .arg(QFileInfo(program).fileName(), tail(output)));
    }
    return output;
}

QString systemRename(const QString& from, const QString& to) {
    if (std::rename(from.toLocal8Bit().constData(),
                    to.toLocal8Bit().constData()) == 0) {
        return {};
    }
    return QString::fromLocal8Bit(std::strerror(errno));
}

// The image while it is mounted; always detached, at the latest at the end of
// the scope.
class Mount {
public:
    Mount() = default;
    Mount(const Mount&) = delete;
    Mount& operator=(const Mount&) = delete;
    ~Mount() { detach(); }

    // `directory` is made by the caller and is ours.
    std::expected<void, QString> attach(const QString& package,
                                        const QString& directory) {
        Directory_ = directory;
        const auto attached =
            run(HDIUTIL,
                {QStringLiteral("attach"), QStringLiteral("-nobrowse"),
                 QStringLiteral("-readonly"), QStringLiteral("-noautoopen"),
                 QStringLiteral("-mountpoint"), directory, package},
                ATTACH_MS);
        if (!attached) {
            // Nothing is mounted by a failed attach.
            QDir().rmdir(directory);
            Directory_.clear();
            return std::unexpected(attached.error());
        }
        Mounted_ = true;
        return {};
    }

    // Detaches, forced if it must be, and removes the directory. A mount that
    // stays is a warning: it does not undo a good install.
    void detach() {
        if (!Mounted_) {
            return;
        }
        Mounted_ = false;
        auto detached =
            run(HDIUTIL, {QStringLiteral("detach"), Directory_}, QUICK_MS);
        if (!detached) {
            detached = run(HDIUTIL,
                           {QStringLiteral("detach"), QStringLiteral("-force"),
                            Directory_},
                           QUICK_MS);
        }
        if (!detached) {
            qWarning("roost: cannot detach the image mounted at %s: %s",
                     qUtf8Printable(Directory_),
                     qUtf8Printable(detached.error()));
            return;
        }
        // rmdir, never a recursive delete: if the volume were still there,
        // this would be its files.
        QDir().rmdir(Directory_);
    }

private:
    QString Directory_;
    bool Mounted_ = false;
};

// Whether the bundle at `path` is a roost.app of `version`: the plist, the
// executables of the window and the daemon, and the version the plist says.
std::expected<void, QString> checkBundle(const QString& path,
                                         const QString& version) {
    const QString plist = path + QStringLiteral("/Contents/Info.plist");
    if (!QFileInfo(plist).isFile()) {
        return std::unexpected(
            QStringLiteral("the copy of roost.app has no Contents/Info.plist"));
    }
    for (const QString& name :
         {QStringLiteral("roost"), QStringLiteral("roostd")}) {
        const QFileInfo file(path + QStringLiteral("/Contents/MacOS/") + name);
        if (!file.isFile() || !file.isExecutable()) {
            return std::unexpected(
                QStringLiteral("the copy of roost.app has no executable "
                               "Contents/MacOS/%1")
                    .arg(name));
        }
    }
    const auto said =
        run(PLIST_BUDDY,
            {QStringLiteral("-c"),
             QStringLiteral("Print :CFBundleShortVersionString"), plist},
            QUICK_MS);
    if (!said) {
        return std::unexpected(
            QStringLiteral("cannot read the version of the copy of roost.app: "
                           "%1")
                .arg(said.error()));
    }
    const QString found = QString::fromUtf8(*said).trimmed();
    if (found != version) {
        return std::unexpected(
            QStringLiteral("the roost.app in the package is version %1, not "
                           "the offered %2")
                .arg(found, version));
    }
    return {};
}

QString manualSteps(const QString& directory) {
    return QStringLiteral(
               "Nothing was changed. To update by hand: download the .dmg from "
               "the release page, open it and drag roost.app into %1, "
               "replacing the old one.")
        .arg(directory);
}

}  // namespace

QString installTarget() {
    // <name>.app/Contents/MacOS/<executable>: the window of an installed copy.
    // A build tree's roost/bin/roost is not one.
    QDir directory(QCoreApplication::applicationDirPath());
    if (directory.dirName() != QStringLiteral("MacOS") || !directory.cdUp() ||
        directory.dirName() != QStringLiteral("Contents") ||
        !directory.cdUp() ||
        !directory.dirName().endsWith(QStringLiteral(".app"))) {
        return {};
    }
    return directory.absolutePath();
}

std::expected<InstallDone, QString> installPackage(
    const InstallRequest& request) {
    const QString target = QDir::cleanPath(request.Target);
    const QFileInfo targetInfo(target);
    if (!target.endsWith(QStringLiteral(".app")) || !targetInfo.isDir()) {
        return std::unexpected(
            QStringLiteral("%1 is not an application bundle").arg(target));
    }
    const QString directory = targetInfo.absolutePath();
    if (!QFileInfo(directory).isWritable()) {
        return std::unexpected(
            QStringLiteral("cannot update %1: %2 is not writable by you. %3")
                .arg(target, directory, manualSteps(directory)));
    }

    // The package sat in the cache since it was verified: once more against
    // its signed line, now that it is about to be used.
    if (const auto same = VerifyFileDigest(request.Package, request.Digest);
        !same) {
        return std::unexpected(
            QStringLiteral("cannot install %1: %2; %3 is unchanged")
                .arg(request.Package, same.error(), target));
    }

    const auto rename = request.Rename ? request.Rename : systemRename;
    const QString id = QUuid::createUuid().toString(QUuid::Id128).left(8);
    const QString base = targetInfo.completeBaseName();
    const QString fresh =
        QStringLiteral("%1/%2-new-%3.app").arg(directory, base, id);
    const QString old =
        QStringLiteral("%1/%2-old-%3.app").arg(directory, base, id);
    const auto dropFresh = [&] { QDir(fresh).removeRecursively(); };

    {
        QTemporaryDir mountDirectory(QFileInfo(request.Package).absolutePath() +
                                     QStringLiteral("/mount-XXXXXX"));
        mountDirectory.setAutoRemove(false);
        if (!mountDirectory.isValid()) {
            return std::unexpected(
                QStringLiteral("cannot make a mount point next to %1: %2; %3 "
                               "is unchanged")
                    .arg(request.Package, mountDirectory.errorString(),
                         target));
        }
        Mount image;
        if (const auto attached =
                image.attach(request.Package, mountDirectory.path());
            !attached) {
            return std::unexpected(
                QStringLiteral("cannot open %1: %2; %3 is unchanged")
                    .arg(request.Package, attached.error(), target));
        }
        const QString source = mountDirectory.path() + QLatin1Char('/') +
                               QLatin1String(APP_IN_IMAGE);
        const QFileInfo sourceInfo(source);
        if (sourceInfo.isSymLink() || !sourceInfo.isDir()) {
            return std::unexpected(
                QStringLiteral("the image %1 has no roost.app; %2 is "
                               "unchanged")
                    .arg(request.Package, target));
        }
        if (const auto copied = run(DITTO, {source, fresh}, COPY_MS); !copied) {
            dropFresh();
            return std::unexpected(
                QStringLiteral("cannot copy roost.app to %1: %2; %3 is "
                               "unchanged")
                    .arg(fresh, copied.error(), target));
        }
        // The copy is on disk: the image is not needed any more.
        image.detach();
    }

    if (const auto good = checkBundle(fresh, request.Version); !good) {
        dropFresh();
        return std::unexpected(QStringLiteral("%1 (%2); %3 is unchanged")
                                   .arg(good.error(), fresh, target));
    }

    if (const QString why = rename(target, old); !why.isEmpty()) {
        dropFresh();
        return std::unexpected(
            QStringLiteral("cannot move %1 to %2: %3; %1 is unchanged")
                .arg(target, old, why));
    }
    if (const QString why = rename(fresh, target); !why.isEmpty()) {
        const QString back = rename(old, target);
        dropFresh();
        if (back.isEmpty()) {
            return std::unexpected(
                QStringLiteral("cannot move %1 to %2: %3; the old %2 is back "
                               "in place")
                    .arg(fresh, target, why));
        }
        return std::unexpected(
            QStringLiteral("cannot move %1 to %2: %3, and the old version "
                           "could not be put back (%4): it is at %5, move it "
                           "to %2 by hand")
                .arg(fresh, target, why, back, old));
    }

    // The old bundle goes to the Trash by the system's API, not deleted: the
    // user keeps what they had. If the Trash refuses it, it stays where it is:
    // the update is in place, which is what counts.
    const auto trash = request.Trash ? request.Trash : [](const QString& path) {
        QString inTrash;
        return QFile::moveToTrash(path, &inTrash) ? inTrash : QString();
    };
    if (const QString inTrash = trash(old); !inTrash.isEmpty()) {
        return InstallDone{.Old = inTrash};
    }
    qWarning(
        "roost: cannot move the old roost.app to the Trash: it stays at %s",
        qUtf8Printable(old));
    return InstallDone{.Old = old};
}

std::expected<void, QString> relaunch(const QString& target) {
    // open -n: LaunchServices starts a new instance of the bundle at this path
    // while this one is still running, with the environment of a launch from
    // the Finder, as the window is made to run in (login_environment.hpp); the
    // package smoke test (tools/package_smoke.sh) starts the window the same
    // way.
    const auto started = run(OPEN, {QStringLiteral("-n"), target}, QUICK_MS);
    if (!started) {
        return std::unexpected(
            QStringLiteral("cannot start %1: %2").arg(target, started.error()));
    }
    return {};
}

}  // namespace roost
