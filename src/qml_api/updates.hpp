#pragma once

#include <QtQml/qqmlregistration.h>
#include <QFuture>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <chrono>
#include <expected>
#include <functional>
#include <optional>

#include "qml_api/update_download.hpp"
#include "qml_api/update_install.hpp"

namespace roost::api {
class Api;
}  // namespace roost::api

namespace roost {

// A release version, numeric X.Y.Z (design/17-updates.md).
struct ReleaseVersion {
    int Major = 0;
    int Minor = 0;
    int Patch = 0;

    friend auto operator<=>(const ReleaseVersion&,
                            const ReleaseVersion&) = default;
};

// "X.Y.Z" or, with `tagged`, "vX.Y.Z"; anything else (a suffix, a missing
// part, a sign, a number too big for an int) is not a version.
std::optional<ReleaseVersion> parseReleaseVersion(const QString& text,
                                                  bool tagged = false);

// The asset of a release that is the package for this platform and arch: the
// .dmg of macOS, the AppImage of Linux (tools/package_macos.sh and
// package_linux.sh name them); empty on any other system. `version` is
// "X.Y.Z".
QString packageAssetName(const QString& version);

// Where the assets of the release `tag` ("vX.Y.Z") are downloaded from, with
// the closing slash: the repository's, or, in a test, the one of the server
// that ROOST_TEST_UPDATES_URL names.
QString assetUrlPrefix(const QString& tag);

// An asset of a release, as the GitHub API lists it.
struct UpdateAsset {
    QString Name;
    QString Url;
};

// Roost.updates: whether a newer release than this one is out (design/17).
// The window asks the GitHub API once it has read the settings, then every 24
// hours; it installs nothing.
//
// `available` is the version ("0.4.0", no "v") of the newest release that is
// newer than this one and than the one the user dismissed; empty when there is
// none, while the state of dismissals is not read, and while the check is off.
// `url` is the page of that release. Both are read-only to QML; `dismiss()`
// hides the offer until a newer version comes.
//
// The package of the offered release is fetched and checked by download()
// (design/17-updates.md): `downloadState` is "" (nothing), "downloading",
// "ready" or "failed", `downloadProgress` runs 0 to 1, `downloadError` is the
// reason of a failure. The verified package is `downloadedPackage()` (C++ only)
// and `packageReady(path)` says it is there; the installer uses these. The
// directory of the package is deleted with the window, by a cancel (a newer
// offer, the check turned off) and by the next download.
//
// install() is the click of "Install and restart" (design/17-updates.md):
// download if the package is not there yet, install it by the installer of the
// platform (update_install.hpp) on a worker thread, start the new version and
// quit this window. `canInstall` is whether this run can do that (a platform
// installer, and a window that is an installed copy); `installState` is ""
// (nothing, or the download is still running), "installing" or "failed", with
// `installError` the reason of a failure of the download or of the install.
// Only the click starts it.
//
// follow() takes the document `settings` (key updates.check, default true:
// false cancels a request in flight and makes no more), followState() the
// document `updates` ({dismissed}); the singleton calls both as it does for
// the theme (RoostSingleton::setApi). Both are C++ only.
class Updates : public QObject {
    Q_OBJECT
    QML_ANONYMOUS

    Q_PROPERTY(QString available READ available NOTIFY changed)
    Q_PROPERTY(QString url READ url NOTIFY changed)
    // Whether the check is on: settings.updates.check.
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    // The settings of this feature that are not valid, one text each; the
    // window shows them in the strip of problems like the font ones.
    Q_PROPERTY(QStringList problems READ problems NOTIFY changed)
    // Whether download() can fetch a package: an offer is shown, and this
    // platform has a package (macOS and Linux do).
    Q_PROPERTY(bool canDownload READ canDownload NOTIFY changed)
    Q_PROPERTY(QString downloadState READ downloadState NOTIFY changed)
    Q_PROPERTY(double downloadProgress READ downloadProgress NOTIFY changed)
    Q_PROPERTY(QString downloadError READ downloadError NOTIFY changed)
    Q_PROPERTY(bool canInstall READ canInstall NOTIFY changed)
    Q_PROPERTY(QString installState READ installState NOTIFY changed)
    Q_PROPERTY(QString installError READ installError NOTIFY changed)
    // Whether an install() is under way: its download or the install itself.
    Q_PROPERTY(bool installBusy READ installBusy NOTIFY changed)

public:
    explicit Updates(QObject* parent = nullptr);
    // An install in progress is waited for: its package is under our directory.
    ~Updates() override;

    // The window's API, for the write of a dismissal. Without one dismiss()
    // hides the offer for this run only. The Api must outlive this object.
    void attach(api::Api& api) { Api_ = &api; }

    // The time between two checks; 24 hours unless a test says otherwise.
    // Takes effect at the next follow() that turns the check on.
    void setInterval(std::chrono::milliseconds interval) {
        Interval_ = interval;
    }

    // For tests: what install() replaces (installTarget() otherwise), how the
    // new version is started, and how this window quits.
    void setInstallTarget(const QString& target) {
        InstallTarget_ = target;
        emit changed();
    }
    void setRelaunch(
        std::function<std::expected<void, QString>(const QString&)> relaunch) {
        Relaunch_ = std::move(relaunch);
    }
    void setQuit(std::function<void()> quit) { Quit_ = std::move(quit); }

    void follow(const QJsonObject& settings);
    void followState(const QJsonObject& document);

    Q_INVOKABLE void dismiss();
    // Downloads the package of the offered release and checks it. Nothing
    // happens without an offer or while a download runs.
    Q_INVOKABLE void download();
    // "Install and restart": downloads the package if it is not there, installs
    // it, starts the new version and quits. Nothing happens without an offer,
    // on a platform or run that cannot install, or while an install is under
    // way.
    Q_INVOKABLE void install();

    QString available() const;
    QString url() const { return available().isEmpty() ? QString() : Url_; }
    bool enabled() const { return Enabled_; }
    const QStringList& problems() const { return Problems_; }
    bool canDownload() const;
    QString downloadState() const;
    double downloadProgress() const { return Download_.progress(); }
    QString downloadError() const { return Download_.error(); }
    bool canInstall() const;
    QString installState() const;
    QString installError() const { return InstallError_; }
    bool installBusy() const {
        return Phase_ == EInstall::Waiting || Phase_ == EInstall::Installing;
    }
    // The verified package once the state is "ready", else empty; C++ only.
    QString downloadedPackage() const { return Download_.packagePath(); }

signals:
    void changed();
    // A request has ended with an answer, good or not (not when it was
    // cancelled). For tests.
    void checked();
    // The package has been downloaded and verified; C++ only.
    void packageReady(const QString& path);
    // The new version is in place; `oldBundle` is where the old one went
    // (the Trash). Before the new version is started. C++ only.
    void installed(const QString& oldBundle);

private:
    void startCheck();
    void cancelCheck();
    void onReply(QNetworkReply* reply);
    void readAnswer(QNetworkReply& reply);
    // Writes a warning for a failure that the last one was not.
    void failed(const QString& reason);
    void succeeded(const QString& version, const QString& url,
                   const QList<UpdateAsset>& assets);
    std::optional<UpdateDownload::Request> downloadRequest();
    // A download is of the release that was offered; but not while it is being
    // installed.
    void cancelDownload();
    void onDownloadChanged();
    void startInstall();
    void onInstalled(const std::expected<InstallDone, QString>& result);

    enum class EInstall { Idle, Waiting, Installing, Failed };

    QNetworkAccessManager Manager_;
    UpdateDownload Download_;
    QTimer Timer_;
    QPointer<QNetworkReply> Reply_;
    api::Api* Api_ = nullptr;
    std::chrono::milliseconds Interval_;
    bool Enabled_ = false;
    // Whether the document `updates` is read: an offer waits for it, or a
    // dismissed version would flash at the start.
    bool StateKnown_ = false;
    QStringList Problems_;
    // The newest release the last answer offered, if it was newer than this
    // one; "X.Y.Z" and its page.
    QString Latest_;
    QString Url_;
    // The assets of that release.
    QList<UpdateAsset> Assets_;
    QString Dismissed_;
    QString LastFailure_;
    EInstall Phase_ = EInstall::Idle;
    QString InstallError_;
    QString InstallTarget_;
    // The version being installed: the offer may change while it is.
    QString InstallVersion_;
    QFuture<std::expected<InstallDone, QString>> Work_;
    std::function<std::expected<void, QString>(const QString&)> Relaunch_;
    std::function<void()> Quit_;
};

}  // namespace roost
