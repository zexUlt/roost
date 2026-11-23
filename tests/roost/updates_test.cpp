// The check for a newer release (design/17-updates.md): the real window, the
// real roostd and the real store; only GitHub is replaced, by a server on the
// loopback that answers what a case says, and ROOST_TEST_UPDATES_URL points the
// check at it. The cases: a newer release is offered, what is not newer or not
// a release is not, a failed request shows nothing and is warned of once,
// settings.updates.check turns the check off, and Dismiss lasts until a newer
// version comes. Then the download of the package of an offered release and its
// check: the same server serves the assets of release v9.9.9 (SHA256SUMS signed
// by the real minisign, tests/fixtures/updates) and a case changes one thing
// in what it serves.

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSemaphore>
#include <QSignalSpy>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUrl>

#include <unistd.h>

#include <atomic>
#include <expected>
#include <memory>
#include <optional>

#include "core/version.hpp"
#include "helpers/scoped_env.hpp"
#include "helpers/sessions_window.hpp"
#include "qml_api/updates.hpp"

using namespace roost;
using namespace roost::testing;

Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

const QString RELEASE_PAGE =
    QStringLiteral("https://github.com/zexUlt/roost/releases/tag/test");
const QRegularExpression FAILED_WARNING(
    QStringLiteral("the check for a newer release failed"));

// The version of this build, "X.Y.Z" of VersionString().
ReleaseVersion Running() {
    const QString text = QString::fromStdString(VersionString());
    return parseReleaseVersion(text.section(u' ', 0, 0)).value();
}

QString Text(const ReleaseVersion& version) {
    return QStringLiteral("%1.%2.%3")
        .arg(version.Major)
        .arg(version.Minor)
        .arg(version.Patch);
}

// A body as the GitHub API answers `releases/latest`.
QByteArray Release(const QString& tag, bool draft = false,
                   bool prerelease = false,
                   const QString& page = RELEASE_PAGE) {
    return QJsonDocument(QJsonObject{{"tag_name", tag},
                                     {"html_url", page},
                                     {"draft", draft},
                                     {"prerelease", prerelease}})
        .toJson(QJsonDocument::Compact);
}

// A server of the loopback that answers each request with the answer it has
// been given. While it holds, a request waits for release(): so a case can
// look at the window before the answer and set a spy on the check.
class ReleaseServer : public QObject {
public:
    explicit ReleaseServer(QObject* parent = nullptr) : QObject(parent) {
        connect(&Server_, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* const socket = Server_.nextPendingConnection()) {
                Accept(socket);
            }
        });
        Listening_ = Server_.listen(QHostAddress::LocalHost, 0);
    }

    bool Listening() const { return Listening_; }
    QString Url() const {
        return QStringLiteral("http://127.0.0.1:%1/releases/latest")
            .arg(Server_.serverPort());
    }
    // The URL of a port that nobody listens at: the connection is refused.
    static QString RefusedUrl() {
        QTcpServer closed;
        closed.listen(QHostAddress::LocalHost, 0);
        const quint16 port = closed.serverPort();
        closed.close();
        return QStringLiteral("http://127.0.0.1:%1/releases/latest").arg(port);
    }

    void Answer(int status, const QByteArray& body) {
        Status_ = status;
        Body_ = body;
    }

    // What a path answers instead of the canned answer: a body, or a redirect
    // to another path, and whether the request waits for ReleaseHeld().
    struct Route {
        int Status = 200;
        QByteArray Body;
        QByteArray Location;
        bool Hold = false;

        static Route Serving(const QByteArray& body) {
            return Route{200, body, {}, false};
        }
        static Route Failing(int status, const QByteArray& body) {
            return Route{status, body, {}, false};
        }
        static Route Redirecting(const QByteArray& location) {
            return Route{302, {}, location, false};
        }
        static Route Waiting(const QByteArray& body) {
            return Route{200, body, {}, true};
        }
    };
    void Serve(const QByteArray& path, const Route& route) {
        Routes_.insert(path, route);
    }
    QString Origin() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(Server_.serverPort());
    }
    // The paths asked for, in the order of the heads.
    const QList<QByteArray>& Asked() const { return Asked_; }
    int AskedFor(const QByteArray& path) const {
        return static_cast<int>(Asked_.count(path));
    }
    void Hold(bool on) { Hold_ = on; }
    int Requests() const { return Requests_; }
    // The requests that wait for release().
    int Held() const { return static_cast<int>(Held_.size()); }
    // The requests whose client closed the connection while it waited.
    int Abandoned() const { return Abandoned_; }
    // The head of the first request.
    const QByteArray& FirstRequest() const { return First_; }

    void ReleaseHeld() {
        const auto held = std::exchange(Held_, {});
        for (const QPointer<QTcpSocket>& socket : held) {
            if (socket != nullptr) {
                Respond(*socket);
            }
        }
    }

private:
    void Accept(QTcpSocket* socket) {
        socket->setParent(this);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            Heads_[socket].append(socket->readAll());
            if (!Heads_[socket].contains("\r\n\r\n")) {
                return;
            }
            if (Requests_++ == 0) {
                First_ = Heads_[socket];
            }
            const QByteArray path = Heads_[socket].split(' ').value(1);
            Asked_.append(path);
            Paths_.insert(socket, path);
            Heads_.remove(socket);
            const auto route = Routes_.constFind(path);
            if (route != Routes_.cend() ? route->Hold : Hold_) {
                Held_.append(socket);
            } else {
                Respond(*socket);
            }
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            if (Held_.removeOne(socket)) {
                ++Abandoned_;
            }
            socket->deleteLater();
        });
    }

    void Respond(QTcpSocket& socket) {
        const auto route = Routes_.constFind(Paths_.take(&socket));
        const bool routed = route != Routes_.cend();
        const int status = routed ? route->Status : Status_;
        const QByteArray& body = routed ? route->Body : Body_;
        const QByteArray location =
            routed && !route->Location.isEmpty()
                ? "Location: " + route->Location + "\r\n"
                : QByteArray();
        socket.write("HTTP/1.1 " + QByteArray::number(status) +
                     " Canned\r\nContent-Type: application/json\r\n" +
                     location +
                     "Content-Length: " + QByteArray::number(body.size()) +
                     "\r\nConnection: close\r\n\r\n" + body);
        socket.disconnectFromHost();
    }

    QTcpServer Server_;
    bool Listening_ = false;
    int Status_ = 200;
    QByteArray Body_ = "{}";
    bool Hold_ = false;
    int Requests_ = 0;
    int Abandoned_ = 0;
    QByteArray First_;
    QHash<QTcpSocket*, QByteArray> Heads_;
    QHash<QTcpSocket*, QByteArray> Paths_;
    QHash<QByteArray, Route> Routes_;
    QList<QByteArray> Asked_;
    QList<QPointer<QTcpSocket>> Held_;
};

}  // namespace

class UpdatesTest : public SessionsWindow {
    Q_OBJECT

private:
    // The server and the variable that points the check at it, for a case.
    void Serve(int status, const QByteArray& body) {
        Server_ = std::make_unique<ReleaseServer>();
        QVERIFY(Server_->Listening());
        Server_->Answer(status, body);
        Url_.emplace("ROOST_TEST_UPDATES_URL", Server_->Url().toUtf8());
    }

    void WriteSettings(const QJsonObject& settings) {
        const QString path = Dir_->filePath("config/settings.json");
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QSaveFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(settings).toJson());
        QVERIFY(file.commit());
    }

    void PatchSettings(const QJsonObject& patch) {
        const QFuture<rpc::Result> writing =
            Api_->Store().Patch(QStringLiteral("settings"), patch);
        QTRY_VERIFY_WITH_TIMEOUT(writing.isFinished(), TIMEOUT_MS);
        QVERIFY(writing.result());
    }

    QJsonObject StoredUpdates() {
        return QJsonDocument::fromJson(
                   ReadFile(Dir_->filePath("config/updates.json")))
            .object();
    }

    QQuickItem* Strip() {
        return Root()->findChild<QQuickItem*>("updateStrip");
    }
    bool StripShown() { return Strip() != nullptr && Strip()->isVisible(); }
    QString Available() {
        return Singleton_->updates()->property("available").toString();
    }

    // The window with the request of the check held at the server: the check
    // is in flight, and `checked` is spied on before its answer.
    // Calls `release()` and waits for the end of the check.
    void BootAndAnswer() {
        Server_->Hold(true);
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        AnswerHeld();
    }

    void AnswerHeld() {
        QTRY_COMPARE_WITH_TIMEOUT(Server_->Held(), 1, TIMEOUT_MS);
        QSignalSpy checked(Singleton_->updates(), &Updates::checked);
        Server_->Hold(false);
        Server_->ReleaseHeld();
        QVERIFY(checked.wait(TIMEOUT_MS) || !checked.isEmpty());
    }

    void ClickItem(QQuickItem* item) {
        QVERIFY(item != nullptr);
        // The layout gives the strip and the button their places after the
        // strip is shown, over more than one polish: until the strip is below
        // the layout, the point may be another item's.
        const auto* layout = Root()->findChild<QQuickItem*>("layoutView");
        QVERIFY(layout != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            layout->height() > 0 &&
                Strip()->y() >= layout->y() + layout->height() &&
                item->width() > 0 && item->height() > 0 &&
                QRectF(QPointF(0, 0), QSizeF(Window()->size()))
                    .contains(item->mapToScene(
                        QPointF(item->width() / 2, item->height() / 2))),
            TIMEOUT_MS);
        QTest::mouseClick(
            Window(), Qt::LeftButton, Qt::NoModifier,
            item->mapToScene(QPointF(item->width() / 2, item->height() / 2))
                .toPoint());
    }

    // ---- the download of a package ------------------------------------

    static QByteArray Fixture(const QString& name) {
        return ReadFile(QStringLiteral(FIXTURES_DIR "/updates/") + name);
    }

    // The assets a release v9.9.9 has: the packages of both platforms and both
    // archs, and the signed sums.
    static QStringList AssetNames() {
        return {"roost-9.9.9-macos-arm64.dmg",
                "roost-9.9.9-macos-x86_64.dmg",
                "roost-x86_64.AppImage",
                "roost-aarch64.AppImage",
                "SHA256SUMS",
                "SHA256SUMS.minisig"};
    }

    // The answer of the releases API: the assets at `base`, which is where the
    // repository's are, unless a case says another.
    QByteArray ReleaseOf(const QString& tag, const QString& base = {},
                         const QStringList& names = AssetNames()) {
        const QString at =
            (base.isEmpty()
                 ? Server_->Origin() + "/releases/download/" + tag + "/"
                 : base);
        QJsonArray assets;
        for (const QString& name : names) {
            assets.append(QJsonObject{{"name", name},
                                      {"browser_download_url", at + name}});
        }
        return QJsonDocument(QJsonObject{{"tag_name", tag},
                                         {"html_url", RELEASE_PAGE},
                                         {"draft", false},
                                         {"prerelease", false},
                                         {"assets", assets}})
            .toJson(QJsonDocument::Compact);
    }

    // What the server serves as the asset `name`: the signed sums and their
    // signature (`signature` is the file of the fixtures), or the package.
    static QByteArray FileOf(const QString& name,
                             const QString& signature = "SHA256SUMS.minisig") {
        if (name == "SHA256SUMS") {
            return Fixture("SHA256SUMS");
        }
        return name == "SHA256SUMS.minisig" ? Fixture(signature)
                                            : PackageFixture(name);
    }

    // The bytes of a package: the .dmg that has a bundle in it for macOS, any
    // bytes for the AppImages (SHA256SUMS lists each by its digest).
    static QByteArray PackageFixture(const QString& name) {
        return Fixture(name.endsWith(".dmg") ? "package-macos.bin"
                                             : "asset.bin");
    }

    // The path of an asset of the release `tag` on the server.
    static QByteArray AssetPath(const QString& tag, const QString& name) {
        return ("/releases/download/" + tag + "/" + name).toUtf8();
    }

    QString PackageName() { return packageAssetName("9.9.9"); }

    // The server with the release v9.9.9 and its files as the release workflow
    // makes them, the key that checks them, and the directory of the download
    // inside the directory of the case. `signature` is the file served as
    // SHA256SUMS.minisig.
    void ServeRelease(const QString& tag = "v9.9.9",
                      const QString& signature = "SHA256SUMS.minisig") {
        Serve(200, {});
        Server_->Answer(200, ReleaseOf(tag));
        Key_.emplace("ROOST_TEST_UPDATES_PUBLIC_KEY", Fixture("test.pub"));
        Cache_.emplace("ROOST_CACHE", Dir_->filePath("cache").toUtf8());
        for (const QString& name : AssetNames()) {
            Server_->Serve(AssetPath(tag, name), ReleaseServer::Route::Serving(
                                                     FileOf(name, signature)));
        }
    }

    // The directories of the attempts of downloads.
    QStringList Attempts() {
        return QDir(Dir_->filePath("cache/updates"))
            .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    }

    // The window is up and the offer of the release is there.
    void BootWithOffer() {
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(Singleton_->updates()->canDownload(),
                                 TIMEOUT_MS);
    }

    QString DownloadState() const {
        return Singleton_->updates()->property("downloadState").toString();
    }
    QString DownloadError() const {
        return Singleton_->updates()->property("downloadError").toString();
    }

    // A download that fails says so in the log, once.
    static void ExpectFailure(const QString& reason) {
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression("roost: the download of the update failed: " +
                               QRegularExpression::escape(reason)));
    }

    // ---- install and restart ------------------------------------------

    // A bundle as far as the installer looks at one: the plist with a
    // version, the two executables, and a file that says whose it is.
    static void MakeBundle(const QString& path, const QString& version,
                           const QByteArray& marker) {
        const auto write = [&](const QString& name, const QByteArray& content,
                               bool executable) {
            const QString file = path + "/" + name;
            QVERIFY(QDir().mkpath(QFileInfo(file).absolutePath()));
            QFile out(file);
            QVERIFY(out.open(QIODevice::WriteOnly));
            out.write(content);
            out.close();
            if (executable) {
                QVERIFY(out.setPermissions(out.permissions() | QFile::ExeOwner |
                                           QFile::ExeGroup | QFile::ExeOther));
            }
        };
        write("Contents/Info.plist",
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist "
              "version=\"1.0\"><dict><key>CFBundleShortVersionString</key>"
              "<string>" +
                  version.toUtf8() + "</string></dict></plist>\n",
              false);
        write("Contents/MacOS/roost", "#!/bin/sh\nexit 0\n", true);
        write("Contents/MacOS/roostd", "#!/bin/sh\nexit 0\n", true);
        write("Contents/Resources/marker.txt", marker, false);
    }

    // `xattr -lr` of a path: the attributes of its files.
    static QString Attributes(const QString& path) {
        QProcess xattr;
        xattr.setProcessChannelMode(QProcess::MergedChannels);
        xattr.start("/usr/bin/xattr", {"-lr", path});
        xattr.waitForFinished(30000);
        return QString::fromUtf8(xattr.readAll());
    }

    QString Installed() const {
        return Dir_->filePath("Applications/roost.app");
    }
    QByteArray InstalledMarker() const {
        return ReadFile(Installed() + "/Contents/Resources/marker.txt");
    }

    // The window with the offer, a bundle "installed" in the directory of the
    // case that the window is told to replace, and the start of the new
    // version and the quit recorded instead of done.
    void BootToInstall() {
        MakeBundle(Installed(), "0.0.1", "old");
        BootToInstallAt(Installed());
    }

    // The same for an AppImage: a file that says whose it is, not yet
    // executable (the install gives it the mode).
    QString InstalledAppImage() const {
        return Dir_->filePath("Applications/roost.AppImage");
    }
    void BootToInstallAppImage() {
        QVERIFY(QDir().mkpath(Dir_->filePath("Applications")));
        QSaveFile file(InstalledAppImage());
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("old appimage\n");
        QVERIFY(file.commit());
        QVERIFY(QFile::setPermissions(InstalledAppImage(),
                                      QFile::ReadOwner | QFile::WriteOwner));
        BootToInstallAt(InstalledAppImage());
    }

    void BootToInstallAt(const QString& target) {
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Updates& updates = *Singleton_->updates();
        updates.setInstallTarget(target);
        updates.setRelaunch([this](const QString& target) {
            Relaunched_.append(target);
            return RelaunchResult_;
        });
        updates.setQuit([this] { ++Quits_; });
        // The old version goes to a Trash of the case, not the user's.
        updates.setTrash([this](const QString& path) {
            const QString trash = Dir_->filePath("Trash");
            const QString to = trash + "/" + QFileInfo(path).fileName();
            return QDir().mkpath(trash) && QDir().rename(path, to) ? to
                                                                   : QString();
        });
        connect(&updates, &Updates::installed, this,
                [this](const QString& old) { Trashed_.append(old); });
        QTRY_VERIFY_WITH_TIMEOUT(updates.canDownload(), TIMEOUT_MS);
    }

    // The Trash of the window held shut: the install stops after the swap and
    // before it ends, so that a case can change things while it "runs".
    // `Reached` is set when the install is there; `Open` lets it go on (it
    // lets any number of Trash moves through).
    struct TrashGate {
        std::atomic<bool> Reached{false};
        QSemaphore Open{0};
        void Let() { Open.release(100); }
    };

    // Lets the install go on when the case ends, however it ends: a stopped
    // install would hold the end of the window.
    struct HeldTrash {
        std::shared_ptr<TrashGate> Gate;
        HeldTrash(const HeldTrash&) = delete;
        HeldTrash& operator=(const HeldTrash&) = delete;
        explicit HeldTrash(std::shared_ptr<TrashGate> gate)
            : Gate(std::move(gate)) {}
        ~HeldTrash() { Gate->Let(); }
        TrashGate* operator->() const { return Gate.get(); }
    };

    HeldTrash HoldTrash() {
        auto gate = std::make_shared<TrashGate>();
        Singleton_->updates()->setTrash([this, gate](const QString& path) {
            gate->Reached.store(true);
            gate->Open.acquire();
            const QString trash = Dir_->filePath("Trash");
            const QString to = trash + "/" + QFileInfo(path).fileName();
            return QDir().mkpath(trash) && QDir().rename(path, to) ? to
                                                                   : QString();
        });
        return HeldTrash(gate);
    }

    void SetCheck(bool on) {
        PatchSettings({{"updates", QJsonObject{{"check", on}}}});
    }

    QQuickItem* InstallButton() {
        return Root()->findChild<QQuickItem*>("updateInstallButton");
    }
    QString StripText() {
        return Root()
            ->findChild<QQuickItem*>("updateText")
            ->property("text")
            .toString();
    }
    QString InstallState() const {
        return Singleton_->updates()->property("installState").toString();
    }

    QStringList Relaunched_;
    std::expected<void, QString> RelaunchResult_;
    int Quits_ = 0;
    QStringList Trashed_;

    std::optional<ScopedEnv> Key_;
    std::optional<ScopedEnv> Cache_;

    std::unique_ptr<ReleaseServer> Server_;
    std::optional<ScopedEnv> Url_;

private slots:
    void init() {
        Prepare();
        FailOnWarnings();
    }

    void cleanup() {
        Trashed_.clear();
        Relaunched_.clear();
        RelaunchResult_ = {};
        Quits_ = 0;
        Teardown();
        Url_.reset();
        Key_.reset();
        Cache_.reset();
        Server_.reset();
    }

    // A tag is vX.Y.Z; the versions are numbers, so 0.10.0 is above 0.9.0.
    void VersionsAreCompared() {
        const auto v = [](const char* text, bool tagged = false) {
            return parseReleaseVersion(QString::fromLatin1(text), tagged);
        };
        QVERIFY(v("0.10.0") > v("0.9.0"));
        QVERIFY(v("1.0.0") > v("0.99.99"));
        QVERIFY(v("0.3.2") > v("0.3.1"));
        QVERIFY(!(v("0.3.1") > v("0.3.1")));
        QVERIFY(v("v1.2.3", true).has_value());
        QVERIFY(!v("1.2.3", true));
        QVERIFY(!v("v1.2.3"));
        for (const char* bad : {"", "1.2", "1.2.3.4", "1.2.3-rc1", "v1.2.3 ",
                                "1.2.x", "-1.2.3", "99999999999.0.0"}) {
            QVERIFY2(!v(bad), bad);
            QVERIFY2(!v(bad, true), bad);
        }
    }

    void ANewerReleaseIsOffered_data() {
        const ReleaseVersion now = Running();
        QTest::addColumn<QString>("tag");
        QTest::addColumn<QString>("shown");
        const auto row = [](const char* name, ReleaseVersion next) {
            QTest::newRow(name) << ("v" + Text(next)) << Text(next);
        };
        row("patch", {now.Major, now.Minor, now.Patch + 1});
        row("minor", {now.Major, now.Minor + 1, 0});
        row("major", {now.Major + 1, 0, 0});
        // A number, not a text: 10 is above 9.
        row("two digits", {now.Major, now.Minor, now.Patch + 10});
    }

    void ANewerReleaseIsOffered() {
        QFETCH(QString, tag);
        QFETCH(QString, shown);
        Serve(200, Release(tag));
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(StripShown(), TIMEOUT_MS);
        QCOMPARE(Strip()
                     ->findChild<QQuickItem*>("updateText")
                     ->property("text")
                     .toString(),
                 QString("roost v%1 is out").arg(shown));
        QCOMPARE(Available(), shown);
        QCOMPARE(Singleton_->updates()->property("url").toString(),
                 RELEASE_PAGE);
        // An offer, not a problem: the strip of problems stays away.
        QVERIFY(Root()->property("shownProblems").toStringList().isEmpty());
        // Just below the layout, above the problems.
        const auto* layout = Root()->findChild<QQuickItem*>("layoutView");
        QVERIFY(layout != nullptr);
        QCOMPARE(Strip()->parentItem(), layout->parentItem());
        QTRY_VERIFY_WITH_TIMEOUT(Strip()->y() >= layout->y() + layout->height(),
                                 TIMEOUT_MS);
        // The way the check asks.
        const QByteArray head = Server_->FirstRequest();
        QVERIFY(head.startsWith("GET /releases/latest "));
        QVERIFY(head.toLower().contains("user-agent: roost/"));
        QVERIFY(head.toLower().contains("accept: application/vnd.github+json"));
    }

    // The link goes to the page of the release; the browser is not started.
    void TheLinkOpensTheReleasePage() {
        Serve(200, Release("v" + Text({Running().Major + 1, 0, 0})));
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(StripShown(), TIMEOUT_MS);
        Root()->setProperty("opensUrls", false);
        QSignalSpy opened(Root(), SIGNAL(releaseOpened(QUrl)));
        ClickItem(Strip()->findChild<QQuickItem*>("updateLinkButton"));
        QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 1, TIMEOUT_MS);
        QCOMPARE(opened.first().at(0).toUrl(), QUrl(RELEASE_PAGE));
    }

    void NothingIsOffered_data() {
        const ReleaseVersion now = Running();
        const QString next = "v" + Text({now.Major + 1, 0, 0});
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("the same") << Release("v" + Text(now));
        QTest::newRow("older") << Release("v0.0.1");
        QTest::newRow("draft") << Release(next, true, false);
        QTest::newRow("prerelease") << Release(next, false, true);
        QTest::newRow("a tag that is no version")
            << Release("nightly-" + Text({now.Major + 1, 0, 0}));
        QTest::newRow("a tag with a suffix") << Release(next + "-rc1");
        QTest::newRow("a tag with two numbers")
            << Release("v" + QString::number(now.Major + 1) + ".0");
        QTest::newRow("a tag without v")
            << Release(Text({now.Major + 1, 0, 0}));
    }

    void NothingIsOffered() {
        QFETCH(QByteArray, body);
        Serve(200, body);
        BootAndAnswer();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(!StripShown());
        QCOMPARE(Available(), QString());
        QCOMPARE(Server_->Requests(), 1);
    }

    void AFailureShowsNothingAndIsWarnedOnce_data() {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QString>("reason");
        QTest::newRow("HTTP 500") << 500 << QByteArray("oops") << "HTTP 500";
        QTest::newRow("HTTP 404")
            << 404 << QByteArray("{\"message\": \"Not Found\"}") << "HTTP 404";
        QTest::newRow("not JSON")
            << 200 << QByteArray("<html>") << "the answer is not a JSON object";
        QTest::newRow("not an object")
            << 200 << QByteArray("[1, 2]") << "the answer is not a JSON object";
        QTest::newRow("no page")
            << 200 << QByteArray("{\"tag_name\": \"v99.0.0\"}")
            << "the answer has no tag_name and html_url";
        QTest::newRow("tag is a number")
            << 200 << QByteArray("{\"tag_name\": 5, \"html_url\": \"x\"}")
            << "the answer has no tag_name and html_url";
        // The page is opened by the system: only the repository's releases.
        const QString next = "v" + Text({Running().Major + 1, 0, 0});
        const char* outside =
            "the answer has a release page outside the "
            "repository";
        QTest::newRow("a file page")
            << 200 << Release(next, false, false, "file:///etc/passwd")
            << outside;
        QTest::newRow("another host")
            << 200 << Release(next, false, false, "https://example.com/x")
            << outside;
        QTest::newRow("a look-alike host")
            << 200
            << Release(next, false, false,
                       "https://github.com.example.com/zexUlt/roost/releases/")
            << outside;
    }

    void AFailureShowsNothingAndIsWarnedOnce() {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(QString, reason);
        Serve(status, body);
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression(FAILED_WARNING.pattern() + ": " +
                               QRegularExpression::escape(reason)));
        BootAndAnswer();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(!StripShown());
        QCOMPARE(Available(), QString());
        QVERIFY(Root()->property("shownProblems").toStringList().isEmpty());
    }

    void AConnectionRefusedIsWarnedOnce() {
        Url_.emplace("ROOST_TEST_UPDATES_URL",
                     ReleaseServer::RefusedUrl().toUtf8());
        Updates updates;
        QSignalSpy checked(&updates, &Updates::checked);
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression(FAILED_WARNING.pattern() + ": network error"));
        updates.follow({});
        QVERIFY(checked.wait(TIMEOUT_MS));
        QCOMPARE(updates.available(), QString());
        QVERIFY(updates.url().isEmpty());
    }

    // The same failure each time is one warning; another reason, or the same
    // after an answer that was good, is another. The check runs again by
    // itself at the interval.
    void ARepeatedFailureIsWarnedOnceAndAGoodAnswerResetsIt() {
        Serve(500, "oops");
        Updates updates;
        updates.setInterval(std::chrono::milliseconds(20));
        QSignalSpy checked(&updates, &Updates::checked);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(": HTTP 500$"));
        updates.follow({});
        QTRY_VERIFY_WITH_TIMEOUT(checked.size() >= 4, TIMEOUT_MS);

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(": HTTP 503$"));
        Server_->Answer(503, "later");
        QTRY_VERIFY_WITH_TIMEOUT(checked.size() >= 8, TIMEOUT_MS);

        Server_->Answer(200, Release("v0.0.1"));
        const qsizetype good = checked.size();
        QTRY_VERIFY_WITH_TIMEOUT(checked.size() >= good + 2, TIMEOUT_MS);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(": HTTP 503$"));
        Server_->Answer(503, "later");
        QTRY_VERIFY_WITH_TIMEOUT(checked.size() >= good + 6, TIMEOUT_MS);
        QVERIFY(Server_->Requests() >= 10);
    }

    // `false` makes no request at all; turning it on makes one.
    void ACheckThatIsOffMakesNoRequest() {
        // uiFontSize is read by the window in the same step as the check: when
        // the font is there, so is the decision about the request.
        constexpr double size = 20;
        WriteSettings(
            {{"updates", QJsonObject{{"check", false}}}, {"uiFontSize", size}});
        Serve(200, Release("v" + Text({Running().Major + 1, 0, 0})));
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(
            Singleton_->theme()->property("uiFont").value<QFont>().pointSizeF(),
            size, TIMEOUT_MS);
        QVERIFY(!Singleton_->updates()->property("enabled").toBool());
        QCOMPARE(Server_->Requests(), 0);
        QVERIFY(!StripShown());

        PatchSettings({{"updates", QJsonObject{{"check", true}}}});
        QTRY_VERIFY_WITH_TIMEOUT(StripShown(), TIMEOUT_MS);
        QCOMPARE(Server_->Requests(), 1);
    }

    // With an offer shown, `false` hides it and `true` shows it again.
    void TurningTheCheckOffHidesTheOffer() {
        Serve(200, Release("v" + Text({Running().Major + 1, 0, 0})));
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(StripShown(), TIMEOUT_MS);
        PatchSettings({{"updates", QJsonObject{{"check", false}}}});
        QTRY_VERIFY_WITH_TIMEOUT(!StripShown(), TIMEOUT_MS);
        QCOMPARE(Available(), QString());

        QSignalSpy checked(Singleton_->updates(), &Updates::checked);
        PatchSettings({{"updates", QJsonObject{{"check", true}}}});
        QVERIFY(checked.wait(TIMEOUT_MS) || !checked.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(StripShown(), TIMEOUT_MS);
        QCOMPARE(Server_->Requests(), 2);
    }

    // A settings document that cannot be read leaves the check on.
    void ASettingsThatCannotBeReadLeavesTheCheckOn() {
        const QString path = Dir_->filePath("config/settings.json");
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QSaveFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{not json");
        QVERIFY(file.commit());
        Serve(200, Release("v" + Text({Running().Major + 1, 0, 0})));
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(StripShown(), TIMEOUT_MS);
        QVERIFY(Singleton_->updates()->property("enabled").toBool());
        QCOMPARE(Server_->Requests(), 1);
    }

    void TurningTheCheckOffCancelsTheRequestInFlight() {
        Serve(200, Release("v" + Text({Running().Major + 1, 0, 0})));
        Server_->Hold(true);
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Server_->Held(), 1, TIMEOUT_MS);
        PatchSettings({{"updates", QJsonObject{{"check", false}}}});
        QTRY_COMPARE_WITH_TIMEOUT(Server_->Abandoned(), 1, TIMEOUT_MS);
        QVERIFY(!StripShown());
        // Off: the window keeps quiet.
        QCOMPARE(Server_->Requests(), 1);
        QVERIFY(!Singleton_->updates()->property("enabled").toBool());
    }

    // A value that is no bool is the default, and a problem the window says.
    void ACheckThatIsNoBoolIsOnAndAProblem_data() {
        QTest::addColumn<QJsonValue>("value");
        QTest::addColumn<bool>("isSection");
        QTest::addColumn<QString>("problem");
        QTest::newRow("a string")
            << QJsonValue("no") << false
            << "settings.updates.check must be true or false; the check is on";
        QTest::newRow("a number")
            << QJsonValue(0) << false
            << "settings.updates.check must be true or false; the check is on";
        QTest::newRow("the section is no object")
            << QJsonValue("off") << true
            << "settings.updates must be an object with check; the check is "
               "on";
    }

    void ACheckThatIsNoBoolIsOnAndAProblem() {
        QFETCH(QJsonValue, value);
        QFETCH(bool, isSection);
        QFETCH(QString, problem);
        WriteSettings(
            {{"updates",
              isSection ? value : QJsonValue(QJsonObject{{"check", value}})}});
        Serve(200, Release("v" + Text({Running().Major + 1, 0, 0})));
        QTest::ignoreMessage(
            QtWarningMsg, QRegularExpression(
                              QRegularExpression::escape("roost: " + problem)));
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(StripShown(), TIMEOUT_MS);
        QCOMPARE(Server_->Requests(), 1);
        QCOMPARE(Root()->property("shownProblems").toStringList(),
                 QStringList{problem});
    }

    // Dismiss hides the version; it stays hidden after the window and its
    // check start again, and a newer version is offered anew.
    void DismissLastsUntilANewerVersionComes() {
        const ReleaseVersion now = Running();
        const ReleaseVersion first{now.Major + 1, 0, 0};
        const ReleaseVersion second{now.Major + 1, 0, 1};
        Serve(200, Release("v" + Text(first)));
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(StripShown(), TIMEOUT_MS);
        ClickItem(Strip()->findChild<QQuickItem*>("updateDismissButton"));
        QTRY_VERIFY_WITH_TIMEOUT(!StripShown(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(StoredUpdates(),
                                  (QJsonObject{{"dismissed", Text(first)}}),
                                  TIMEOUT_MS);

        // The window starts again: the check finds the same version.
        CloseWindow();
        Server_->Hold(true);
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        AnswerHeld();
        QVERIFY(!StripShown());
        QCOMPARE(Server_->Requests(), 2);

        // A newer one is out.
        CloseWindow();
        Server_->Answer(200, Release("v" + Text(second)));
        Server_->Hold(true);
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        AnswerHeld();
        QVERIFY(StripShown());
        QCOMPARE(Strip()
                     ->findChild<QQuickItem*>("updateText")
                     ->property("text")
                     .toString(),
                 QString("roost v%1 is out").arg(Text(second)));
        // The dismissal of the older one is not touched until it is made.
        QCOMPARE(StoredUpdates(), (QJsonObject{{"dismissed", Text(first)}}));
    }

    // The document `updates` is what the window follows: a dismissal written
    // by another window hides the offer here.
    void ADismissalWrittenElsewhereHidesTheOffer() {
        Serve(200, Release("v" + Text({Running().Major + 1, 0, 0})));
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(StripShown(), TIMEOUT_MS);
        const QFuture<rpc::Result> writing = Api_->Store().Patch(
            QStringLiteral("updates"),
            QJsonObject{{"dismissed", Text({Running().Major + 1, 0, 0})}});
        QTRY_VERIFY_WITH_TIMEOUT(writing.isFinished(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!StripShown(), TIMEOUT_MS);
    }

    // ---- the download of a package --------------------------------------

    // The name of the package of this platform and arch, as the package scripts
    // name it; and where the files of the repository are.
    void ThePackageAndTheRepositoryAreNamed() {
#if defined(Q_OS_MACOS)
        const QString arch = QSysInfo::buildCpuArchitecture();
        QVERIFY(arch == "arm64" || arch == "x86_64");
        QCOMPARE(packageAssetName("1.2.3"),
                 QString("roost-1.2.3-macos-%1.dmg").arg(arch));
#elif defined(Q_OS_LINUX)
        QCOMPARE(packageAssetName("1.2.3"),
                 QSysInfo::buildCpuArchitecture() == "x86_64"
                     ? QString("roost-x86_64.AppImage")
                     : QString("roost-aarch64.AppImage"));
#endif
        const ScopedEnv noServer("ROOST_TEST_UPDATES_URL", QByteArray());
        QCOMPARE(assetUrlPrefix("v1.2.3"),
                 QString("https://github.com/zexUlt/roost/releases/download/"
                         "v1.2.3/"));
        const ScopedEnv server(
            "ROOST_TEST_UPDATES_URL",
            QByteArray("http://127.0.0.1:8/releases/latest"));
        QCOMPARE(assetUrlPrefix("v1.2.3"),
                 QString("http://127.0.0.1:8/releases/download/v1.2.3/"));
    }

    // The package, SHA256SUMS and its signature are downloaded, checked, and
    // left in a directory of their own; the window is told.
    void AGoodPackageIsReady() {
        ServeRelease();
        BootWithOffer();
        if (QTest::currentTestFailed()) {
            return;
        }
        Updates& updates = *Singleton_->updates();
        QCOMPARE(DownloadState(), QString());
        QSignalSpy ready(&updates, &Updates::packageReady);
        updates.download();
        QTRY_COMPARE_WITH_TIMEOUT(DownloadState(), QString("ready"),
                                  TIMEOUT_MS);
        QCOMPARE(ready.size(), 1);
        const QString path = ready.first().at(0).toString();
        QCOMPARE(updates.downloadedPackage(), path);
        QCOMPARE(ReadFile(path), PackageFixture(PackageName()));
        QCOMPARE(QFileInfo(path).fileName(), PackageName());
        // Under the cache directory, in a directory made for this attempt.
        QCOMPARE(Attempts().size(), 1);
        QCOMPARE(QFileInfo(path).absolutePath(),
                 Dir_->filePath("cache/updates/" + Attempts().first()));
        QCOMPARE(ReadFile(Dir_->filePath("cache/updates/" + Attempts().first() +
                                         "/SHA256SUMS")),
                 Fixture("SHA256SUMS"));
        QCOMPARE(updates.property("downloadProgress").toDouble(), 1.0);
        QVERIFY(DownloadError().isEmpty());
        // The three files, once each.
        QCOMPARE(Server_->AskedFor(AssetPath("v9.9.9", PackageName())), 1);
        QCOMPARE(Server_->AskedFor(AssetPath("v9.9.9", "SHA256SUMS")), 1);
        QCOMPARE(Server_->AskedFor(AssetPath("v9.9.9", "SHA256SUMS.minisig")),
                 1);
        // A ready package is not fetched again: another call keeps it.
        updates.download();
        QCOMPARE(DownloadState(), QString("ready"));
        QCOMPARE(updates.downloadedPackage(), path);
        QVERIFY(QFileInfo::exists(path));
        QCOMPARE(Server_->AskedFor(AssetPath("v9.9.9", PackageName())), 1);
        QCOMPARE(ready.size(), 1);
        // With the window the directory goes.
        CloseWindow();
        QVERIFY(Attempts().isEmpty());
    }

    // The storage of GitHub is another address: the download follows the
    // redirect, the check is the same.
    void ARedirectToTheStorageIsFollowed() {
        ServeRelease();
        const QByteArray storage = "/storage/package?token=abc";
        Server_->Serve(storage, ReleaseServer::Route::Serving(
                                    PackageFixture(PackageName())));
        Server_->Serve(AssetPath("v9.9.9", PackageName()),
                       ReleaseServer::Route::Redirecting(storage));
        BootWithOffer();
        if (QTest::currentTestFailed()) {
            return;
        }
        QSignalSpy ready(Singleton_->updates(), &Updates::packageReady);
        Singleton_->updates()->download();
        QTRY_COMPARE_WITH_TIMEOUT(DownloadState(), QString("ready"),
                                  TIMEOUT_MS);
        QCOMPARE(ReadFile(ready.first().at(0).toString()),
                 PackageFixture(PackageName()));
        QCOMPARE(Server_->AskedFor(storage), 1);
    }

    // A package that is not the one signed in SHA256SUMS: failed, the reason
    // is there, and nothing is kept.
    void ATamperedPackageFails() {
        ServeRelease();
        QByteArray tampered = PackageFixture(PackageName());
        tampered[tampered.size() / 2] =
            static_cast<char>(tampered[tampered.size() / 2] ^ 0x01);
        Server_->Serve(AssetPath("v9.9.9", PackageName()),
                       ReleaseServer::Route::Serving(tampered));
        BootWithOffer();
        if (QTest::currentTestFailed()) {
            return;
        }
        QSignalSpy ready(Singleton_->updates(), &Updates::packageReady);
        ExpectFailure("the package does not match its line in SHA256SUMS");
        Singleton_->updates()->download();
        QTRY_COMPARE_WITH_TIMEOUT(DownloadState(), QString("failed"),
                                  TIMEOUT_MS);
        QCOMPARE(DownloadError(),
                 QString("the package does not match its line in SHA256SUMS"));
        QVERIFY(Singleton_->updates()->downloadedPackage().isEmpty());
        QVERIFY(ready.isEmpty());
        QVERIFY(Attempts().isEmpty());
        // The package was fetched: the check of the sums did not stop it.
        QCOMPARE(Server_->AskedFor(AssetPath("v9.9.9", PackageName())), 1);
        // A new attempt starts clean.
        Server_->Serve(
            AssetPath("v9.9.9", PackageName()),
            ReleaseServer::Route::Serving(PackageFixture(PackageName())));
        Singleton_->updates()->download();
        QTRY_COMPARE_WITH_TIMEOUT(DownloadState(), QString("ready"),
                                  TIMEOUT_MS);
        QVERIFY(DownloadError().isEmpty());
    }

    // What is wrong with the signed sums stops the download before the package
    // is fetched.
    void ASumsThatAreNotSignedRightFail_data() {
        QTest::addColumn<QString>("signature");
        QTest::addColumn<QString>("reason");
        QTest::newRow("another key") << "SHA256SUMS.other.minisig"
                                     << "the signature is by another key";
    }

    void ASumsThatAreNotSignedRightFail() {
        QFETCH(QString, signature);
        QFETCH(QString, reason);
        ServeRelease("v9.9.9", signature);
        BootWithOffer();
        if (QTest::currentTestFailed()) {
            return;
        }
        ExpectFailure(reason);
        Singleton_->updates()->download();
        QTRY_COMPARE_WITH_TIMEOUT(DownloadState(), QString("failed"),
                                  TIMEOUT_MS);
        QVERIFY(DownloadError().startsWith(reason));
        QVERIFY(Attempts().isEmpty());
        QCOMPARE(Server_->AskedFor(AssetPath("v9.9.9", PackageName())), 0);
    }

    // The same files signed for another release do not pass for this one.
    void TheSignedFilesOfAnotherReleaseFail() {
        ServeRelease("v9.9.9");
        // The release offered is v9.9.10, whose files are those of v9.9.9.
        QStringList names = AssetNames();
        names.append(packageAssetName("9.9.10"));
        for (const QString& name : names) {
            Server_->Serve(AssetPath("v9.9.10", name),
                           ReleaseServer::Route::Serving(FileOf(name)));
        }
        Server_->Answer(200, ReleaseOf("v9.9.10", {}, names));
        BootWithOffer();
        if (QTest::currentTestFailed()) {
            return;
        }
        ExpectFailure("the signed comment is");
        Singleton_->updates()->download();
        QTRY_COMPARE_WITH_TIMEOUT(DownloadState(), QString("failed"),
                                  TIMEOUT_MS);
        QVERIFY(DownloadError().contains("roost v9.9.9 SHA256SUMS"));
        QVERIFY(Attempts().isEmpty());
    }

    // The key built into the app is the one the files are checked with, unless
    // a test names another: the files of the fixtures are signed by a key of
    // the tests, not by the key of the releases.
    void TheBuiltInKeyIsTheOneThatChecks() {
        ServeRelease();
        Key_.reset();
        BootWithOffer();
        if (QTest::currentTestFailed()) {
            return;
        }
        ExpectFailure("the signature is by another key");
        Singleton_->updates()->download();
        QTRY_COMPARE_WITH_TIMEOUT(DownloadState(), QString("failed"),
                                  TIMEOUT_MS);
        QVERIFY(Attempts().isEmpty());
    }

    // An address of a file that is not in the repository's releases: nothing
    // is fetched.
    void AnAssetOutsideTheRepositoryIsNotFetched_data() {
        QTest::addColumn<QString>("package");
        QTest::addColumn<QString>("sums");
        const QString any = "OTHER";
        QTest::newRow("another path")
            << "/other/package" << "/releases/download/v9.9.9/SHA256SUMS";
        QTest::newRow("another release")
            << "/releases/download/v9.9.8/package"
            << "/releases/download/v9.9.9/SHA256SUMS";
        QTest::newRow("a path up") << "/releases/download/v9.9.9/../../x"
                                   << "/releases/download/v9.9.9/SHA256SUMS";
        QTest::newRow("a query") << "/releases/download/v9.9.9/package?x=1"
                                 << "/releases/download/v9.9.9/SHA256SUMS";
        QTest::newRow("an escaped path")
            << "/releases/download/v9.9.9/%2e%2e/x"
            << "/releases/download/v9.9.9/SHA256SUMS";
        QTest::newRow("a sums file elsewhere")
            << "/releases/download/v9.9.9/package" << "/elsewhere/SHA256SUMS";
        QTest::newRow("a file URL") << "file:///etc/passwd"
                                    << "/releases/download/v9.9.9/SHA256SUMS";
    }

    void AnAssetOutsideTheRepositoryIsNotFetched() {
        QFETCH(QString, package);
        QFETCH(QString, sums);
        ServeRelease();
        const auto address = [&](const QString& path) {
            return path.startsWith("file:") ? path : Server_->Origin() + path;
        };
        QJsonArray assets;
        const auto add = [&](const QString& name, const QString& url) {
            assets.append(
                QJsonObject{{"name", name}, {"browser_download_url", url}});
        };
        const QString prefix = Server_->Origin() + "/releases/download/v9.9.9/";
        add(PackageName(), address(package.replace("package", PackageName())));
        add("SHA256SUMS", address(sums));
        add("SHA256SUMS.minisig", prefix + "SHA256SUMS.minisig");
        Server_->Answer(200,
                        QJsonDocument(QJsonObject{{"tag_name", "v9.9.9"},
                                                  {"html_url", RELEASE_PAGE},
                                                  {"assets", assets}})
                            .toJson(QJsonDocument::Compact));
        BootWithOffer();
        if (QTest::currentTestFailed()) {
            return;
        }
        ExpectFailure("the release has");
        Singleton_->updates()->download();
        QTRY_COMPARE_WITH_TIMEOUT(DownloadState(), QString("failed"),
                                  TIMEOUT_MS);
        QVERIFY2(DownloadError().contains("outside the repository"),
                 qPrintable(DownloadError()));
        QVERIFY(Attempts().isEmpty());
        // Nothing but the check was asked.
        QCOMPARE(Server_->Asked(), QList<QByteArray>{"/releases/latest"});
    }

    // A release without a file the download needs fails with the name of it.
    void AReleaseWithoutAFileFails_data() {
        QTest::addColumn<QString>("missing");
        QTest::newRow("the package") << PackageName();
        QTest::newRow("the sums") << "SHA256SUMS";
        QTest::newRow("the signature") << "SHA256SUMS.minisig";
    }

    void AReleaseWithoutAFileFails() {
        QFETCH(QString, missing);
        ServeRelease();
        QStringList names = AssetNames();
        names.removeAll(missing);
        Server_->Answer(200, ReleaseOf("v9.9.9", {}, names));
        BootWithOffer();
        if (QTest::currentTestFailed()) {
            return;
        }
        ExpectFailure("the release has no file " + missing);
        Singleton_->updates()->download();
        QTRY_COMPARE_WITH_TIMEOUT(DownloadState(), QString("failed"),
                                  TIMEOUT_MS);
        QVERIFY(Attempts().isEmpty());
        QCOMPARE(Server_->Asked(), QList<QByteArray>{"/releases/latest"});
    }

    // A server that says no, and one that sends more than the limit.
    void AServerThatSaysNoFails() {
        ServeRelease();
        Server_->Serve(AssetPath("v9.9.9", "SHA256SUMS"),
                       ReleaseServer::Route::Failing(404, "Not Found"));
        BootWithOffer();
        if (QTest::currentTestFailed()) {
            return;
        }
        ExpectFailure("cannot download SHA256SUMS: HTTP 404");
        Singleton_->updates()->download();
        QTRY_COMPARE_WITH_TIMEOUT(DownloadState(), QString("failed"),
                                  TIMEOUT_MS);
        QVERIFY(Attempts().isEmpty());
    }

    void ASumsFileThatIsTooBigFails() {
        ServeRelease();
        Server_->Serve(
            AssetPath("v9.9.9", "SHA256SUMS"),
            ReleaseServer::Route::Serving(QByteArray((1 << 20) + 1, 'x')));
        BootWithOffer();
        if (QTest::currentTestFailed()) {
            return;
        }
        ExpectFailure("SHA256SUMS is larger than");
        Singleton_->updates()->download();
        QTRY_COMPARE_WITH_TIMEOUT(DownloadState(), QString("failed"),
                                  TIMEOUT_MS);
        QVERIFY(Attempts().isEmpty());
    }

    // The check turned off ends a download in flight, and its directory goes.
    void TurningTheCheckOffCancelsTheDownload() {
        ServeRelease();
        Server_->Serve(
            AssetPath("v9.9.9", PackageName()),
            ReleaseServer::Route::Waiting(PackageFixture(PackageName())));
        BootWithOffer();
        if (QTest::currentTestFailed()) {
            return;
        }
        QSignalSpy ready(Singleton_->updates(), &Updates::packageReady);
        Singleton_->updates()->download();
        QTRY_COMPARE_WITH_TIMEOUT(Server_->Held(), 1, TIMEOUT_MS);
        QCOMPARE(DownloadState(), QString("downloading"));
        QCOMPARE(Attempts().size(), 1);
        PatchSettings({{"updates", QJsonObject{{"check", false}}}});
        QTRY_COMPARE_WITH_TIMEOUT(Server_->Abandoned(), 1, TIMEOUT_MS);
        QCOMPARE(DownloadState(), QString());
        QVERIFY(Attempts().isEmpty());
        QVERIFY(ready.isEmpty());
        QVERIFY(Singleton_->updates()->downloadedPackage().isEmpty());
        QVERIFY(!Singleton_->updates()->canDownload());
    }

    // A newer release than the one being downloaded ends the download too.
    void ANewerOfferCancelsTheDownload() {
        ServeRelease();
        Server_->Serve(
            AssetPath("v9.9.9", PackageName()),
            ReleaseServer::Route::Waiting(PackageFixture(PackageName())));
        Updates updates;
        updates.setInterval(std::chrono::milliseconds(50));
        updates.followState({});
        updates.follow({});
        QTRY_VERIFY_WITH_TIMEOUT(updates.canDownload(), TIMEOUT_MS);
        updates.download();
        QTRY_COMPARE_WITH_TIMEOUT(Server_->Held(), 1, TIMEOUT_MS);
        QCOMPARE(updates.downloadState(), QString("downloading"));
        // The same release, asked again by the timer, does not end it.
        const int asked = Server_->AskedFor("/releases/latest");
        QTRY_VERIFY_WITH_TIMEOUT(Server_->AskedFor("/releases/latest") > asked,
                                 TIMEOUT_MS);
        QCOMPARE(updates.downloadState(), QString("downloading"));
        // A newer one is out.
        Server_->Answer(200, ReleaseOf("v9.9.10"));
        QTRY_COMPARE_WITH_TIMEOUT(Server_->Abandoned(), 1, TIMEOUT_MS);
        QCOMPARE(updates.downloadState(), QString());
        QVERIFY(Attempts().isEmpty());
        QCOMPARE(updates.available(), QString("9.9.10"));
    }

    // Without an offer there is nothing to download.
    void WithoutAnOfferThereIsNothingToDownload() {
        ServeRelease("v0.0.1");
        Server_->Answer(200, ReleaseOf("v0.0.1"));
        BootAndAnswer();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(!Singleton_->updates()->canDownload());
        Singleton_->updates()->download();
        QCOMPARE(DownloadState(), QString());
        QCOMPARE(Server_->Asked(), QList<QByteArray>{"/releases/latest"});
    }

    // Where the run is no installed copy (a build tree, Linux), the strip has
    // the link to the release and no install button, and install() is nothing.
    void WithoutAnInstallableRunThereIsOnlyTheLink() {
        ServeRelease();
        BootWithOffer();
        if (QTest::currentTestFailed()) {
            return;
        }
        // The test binary is in a build tree, not in an installed copy.
        Updates& updates = *Singleton_->updates();
        QVERIFY(!updates.canInstall());
        QTRY_VERIFY_WITH_TIMEOUT(StripShown(), TIMEOUT_MS);
        QVERIFY(
            Root()->findChild<QQuickItem*>("updateLinkButton")->isVisible());
        QVERIFY(!InstallButton()->isVisible());
        updates.install();
        QCOMPARE(DownloadState(), QString());
        QCOMPARE(InstallState(), QString());
        QCOMPARE(Server_->AskedFor(AssetPath("v9.9.9", PackageName())), 0);
    }

#ifdef Q_OS_LINUX
    // The click on Linux: the AppImage is replaced by the package (the bytes
    // the signed SHA256SUMS lists), it is executable, nothing is left beside
    // it, the new one is started at its path and this window quits once.
    void TheClickReplacesTheAppImageAndRestarts() {
        ServeRelease();
        BootToInstallAppImage();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(InstallButton()->isVisible(), TIMEOUT_MS);
        QCOMPARE(ReadFile(InstalledAppImage()), QByteArray("old appimage\n"));
        QVERIFY(Relaunched_.isEmpty());

        ClickItem(InstallButton());
        QTRY_COMPARE_WITH_TIMEOUT(Quits_, 1, TIMEOUT_MS);
        QCOMPARE(ReadFile(InstalledAppImage()), PackageFixture(PackageName()));
        QVERIFY(QFileInfo(InstalledAppImage()).isExecutable());
        QCOMPARE(Relaunched_, QStringList{InstalledAppImage()});
        // No old copy is kept: the signal says there is none.
        QCOMPARE(Trashed_, QStringList{QString()});
        QCOMPARE(QDir(Dir_->filePath("Applications")).entryList(QDir::Files),
                 QStringList{"roost.AppImage"});
        QCOMPARE(Server_->AskedFor(AssetPath("v9.9.9", PackageName())), 1);
        QCOMPARE(StripText(), QString("Installing roost v9.9.9..."));
    }

    // A directory that is not writable: the strip says where and what to do
    // by hand, the AppImage is as it was, and the click tries again.
    void AnAppImageInAReadOnlyDirectoryIsNotReplaced() {
        if (::geteuid() == 0) {
            QSKIP("root writes through directory permissions");
        }
        ServeRelease();
        BootToInstallAppImage();
        if (QTest::currentTestFailed()) {
            return;
        }
        const QString directory = Dir_->filePath("Applications");
        QVERIFY(QFile::setPermissions(directory,
                                      QFile::ReadOwner | QFile::ExeOwner));
        QTRY_VERIFY_WITH_TIMEOUT(InstallButton()->isVisible(), TIMEOUT_MS);
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression("roost: the install of the update failed: "
                               "cannot create a file in .*Permission denied"));
        ClickItem(InstallButton());
        QTRY_COMPARE_WITH_TIMEOUT(InstallState(), QString("failed"),
                                  TIMEOUT_MS);
        QVERIFY(StripText().contains(directory));
        QVERIFY(StripText().contains("replace the file by hand"));
        QCOMPARE(ReadFile(InstalledAppImage()), QByteArray("old appimage\n"));
        QVERIFY(Relaunched_.isEmpty());
        QCOMPARE(Quits_, 0);
        QVERIFY(InstallButton()->isEnabled());
        // The package stays: the second try installs it without fetching.
        QVERIFY(QFile::setPermissions(
            directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        ClickItem(InstallButton());
        QTRY_COMPARE_WITH_TIMEOUT(Quits_, 1, TIMEOUT_MS);
        QCOMPARE(ReadFile(InstalledAppImage()), PackageFixture(PackageName()));
        QCOMPARE(Server_->AskedFor(AssetPath("v9.9.9", PackageName())), 1);
    }
#endif

#ifdef Q_OS_MACOS
    // The click is download, install, start the new version, quit; nothing
    // happens before it.
    void TheClickInstallsAndRestarts() {
        ServeRelease();
        BootToInstall();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(InstallButton()->isVisible(), TIMEOUT_MS);
        QCOMPARE(InstallButton()->property("text").toString(),
                 QString("Install and restart"));
        // An offer alone fetches nothing and changes nothing.
        QCOMPARE(DownloadState(), QString());
        QCOMPARE(Server_->AskedFor(AssetPath("v9.9.9", PackageName())), 0);
        QCOMPARE(InstalledMarker(), QByteArray("old"));
        QVERIFY(Relaunched_.isEmpty());

        ClickItem(InstallButton());
        QTRY_COMPARE_WITH_TIMEOUT(Quits_, 1, TIMEOUT_MS);
        // The new bundle is in place, started, and this window quit once.
        QCOMPARE(InstalledMarker(), QByteArray("new"));
        QCOMPARE(Relaunched_, QStringList{Installed()});
        QCOMPARE(Trashed_.size(), 1);
        QVERIFY(Trashed_.first().startsWith(Dir_->filePath("Trash") + "/"));
        QCOMPARE(ReadFile(Trashed_.first() + "/Contents/Resources/marker.txt"),
                 QByteArray("old"));
        QCOMPARE(QDir(Dir_->filePath("Applications"))
                     .entryList(QDir::Dirs | QDir::NoDotAndDotDot),
                 QStringList{"roost.app"});
        QCOMPARE(Server_->AskedFor(AssetPath("v9.9.9", PackageName())), 1);
        QCOMPARE(StripText(), QString("Installing roost v9.9.9..."));
        // A package the window downloads itself has no quarantine attribute,
        // so the installer has no xattr step (design/17-updates.md).
        QVERIFY(!Attributes(Dir_->filePath("cache/updates"))
                     .contains("com.apple.quarantine"));
        QVERIFY(!InstallButton()->isEnabled());
        // The package is deleted with the window.
        CloseWindow();
        QVERIFY(Attempts().isEmpty());
    }

    // While the package comes, the strip says so with the share received, the
    // button and Dismiss are off, and the install follows the download.
    void TheStripShowsTheDownloadAndKeepsTheOfferUntilItEnds() {
        ServeRelease();
        Server_->Serve(
            AssetPath("v9.9.9", PackageName()),
            ReleaseServer::Route::Waiting(PackageFixture(PackageName())));
        BootToInstall();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(InstallButton()->isVisible(), TIMEOUT_MS);
        ClickItem(InstallButton());
        QTRY_COMPARE_WITH_TIMEOUT(Server_->Held(), 1, TIMEOUT_MS);
        QVERIFY(StripText().startsWith("Downloading roost v9.9.9"));
        QVERIFY(!InstallButton()->isEnabled());
        QVERIFY(!Root()
                     ->findChild<QQuickItem*>("updateDismissButton")
                     ->isEnabled());
        // Dismiss is nothing while the install is under way.
        Singleton_->updates()->dismiss();
        QCOMPARE(Available(), QString("9.9.9"));
        // Another click does not start a second one.
        Singleton_->updates()->install();
        QCOMPARE(Server_->AskedFor(AssetPath("v9.9.9", PackageName())), 1);
        Server_->Hold(false);
        Server_->ReleaseHeld();
        QTRY_COMPARE_WITH_TIMEOUT(Quits_, 1, TIMEOUT_MS);
        QCOMPARE(InstalledMarker(), QByteArray("new"));
    }

    // An install that fails says why on the strip, changes nothing, and the
    // button tries again: here the directory of the bundle is not writable,
    // then it is.
    void AFailedInstallIsShownAndCanBeTriedAgain() {
        if (::geteuid() == 0) {
            QSKIP("root writes through directory permissions");
        }
        ServeRelease();
        BootToInstall();
        if (QTest::currentTestFailed()) {
            return;
        }
        const QString directory = Dir_->filePath("Applications");
        QVERIFY(QFile::setPermissions(directory,
                                      QFile::ReadOwner | QFile::ExeOwner));
        QTRY_VERIFY_WITH_TIMEOUT(InstallButton()->isVisible(), TIMEOUT_MS);
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression("roost: the install of the update failed: "
                               "cannot update .* is not writable by you"));
        ClickItem(InstallButton());
        QTRY_COMPARE_WITH_TIMEOUT(InstallState(), QString("failed"),
                                  TIMEOUT_MS);
        QVERIFY(StripText().contains(directory + " is not writable"));
        QVERIFY(StripText().contains("drag roost.app into " + directory));
        QCOMPARE(InstalledMarker(), QByteArray("old"));
        QVERIFY(Relaunched_.isEmpty());
        QCOMPARE(Quits_, 0);
        QVERIFY(InstallButton()->isEnabled());
        // The package stays: the second try installs it without fetching.
        QCOMPARE(DownloadState(), QString("ready"));
        QVERIFY(QFile::setPermissions(
            directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        ClickItem(InstallButton());
        QTRY_COMPARE_WITH_TIMEOUT(Quits_, 1, TIMEOUT_MS);
        QCOMPARE(InstalledMarker(), QByteArray("new"));
        QCOMPARE(Server_->AskedFor(AssetPath("v9.9.9", PackageName())), 1);
    }

    // A new version that is installed but cannot be started is said so, with
    // what to do; the window stays.
    void ANewVersionThatCannotBeStartedIsShown() {
        ServeRelease();
        BootToInstall();
        if (QTest::currentTestFailed()) {
            return;
        }
        RelaunchResult_ = std::unexpected(QString("no way"));
        QTRY_VERIFY_WITH_TIMEOUT(InstallButton()->isVisible(), TIMEOUT_MS);
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression(
                "roost: roost 9.9.9 is installed, but this window "
                "cannot start it: no way"));
        ClickItem(InstallButton());
        QTRY_COMPARE_WITH_TIMEOUT(InstallState(), QString("failed"),
                                  TIMEOUT_MS);
        QCOMPARE(Relaunched_, QStringList{Installed()});
        QCOMPARE(Quits_, 0);
        QVERIFY(StripText().contains("roost 9.9.9 is installed"));
        QVERIFY(StripText().contains("no way"));
        QCOMPARE(InstalledMarker(), QByteArray("new"));
    }

    // A download that fails is the failure of the install: nothing is
    // replaced.
    void AFailedDownloadIsShownOnTheInstallStrip() {
        ServeRelease();
        Server_->Serve(AssetPath("v9.9.9", PackageName()),
                       ReleaseServer::Route::Failing(404, "gone"));
        BootToInstall();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(InstallButton()->isVisible(), TIMEOUT_MS);
        ExpectFailure("cannot download " + PackageName() + ": HTTP 404");
        ClickItem(InstallButton());
        QTRY_COMPARE_WITH_TIMEOUT(InstallState(), QString("failed"),
                                  TIMEOUT_MS);
        QVERIFY(StripText().contains("cannot download " + PackageName()));
        QCOMPARE(InstalledMarker(), QByteArray("old"));
        QCOMPARE(Quits_, 0);
        QVERIFY(InstallButton()->isEnabled());
    }
    // A download that fails at once (the release has no .dmg) says the same
    // thing every time, and UpdateDownload does not say the same thing twice:
    // the second click must still end in a visible state.
    void ASecondClickAfterAnImmediateFailureEndsInTheFailure() {
        ServeRelease();
        QStringList names = AssetNames();
        names.removeAll(PackageName());
        Server_->Answer(200, ReleaseOf("v9.9.9", {}, names));
        BootToInstall();
        if (QTest::currentTestFailed()) {
            return;
        }
        const QRegularExpression warning(
            "roost: the download of the update failed: the release has no "
            "file " +
            QRegularExpression::escape(PackageName()));
        QTest::ignoreMessage(QtWarningMsg, warning);
        QTest::ignoreMessage(QtWarningMsg, warning);
        QTRY_VERIFY_WITH_TIMEOUT(InstallButton()->isVisible(), TIMEOUT_MS);
        ClickItem(InstallButton());
        QTRY_COMPARE_WITH_TIMEOUT(InstallState(), QString("failed"),
                                  TIMEOUT_MS);
        QVERIFY(StripText().contains("the release has no file"));
        // Moved on by hand: the second click starts from nothing.
        QVERIFY(InstallButton()->isEnabled());
        ClickItem(InstallButton());
        QTRY_COMPARE_WITH_TIMEOUT(InstallState(), QString("failed"),
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!Singleton_->updates()->installBusy(),
                                 TIMEOUT_MS);
        QVERIFY(StripText().contains("the release has no file"));
        QVERIFY(InstallButton()->isEnabled());
        QVERIFY(
            Root()->findChild<QQuickItem*>("updateDismissButton")->isEnabled());
        QCOMPARE(Quits_, 0);
    }

    // A newer release comes while the install runs: the strip still says what
    // is being installed, and when the install ends (here: the new version
    // cannot be started) the package of the old offer is not used for the new
    // one: the next click fetches the package of the new offer.
    void ANewOfferDuringTheInstallDoesNotKeepTheOldPackage() {
        ServeRelease();
        QStringList names = AssetNames();
        names.append(packageAssetName("9.9.10"));
        for (const QString& name : names) {
            Server_->Serve(AssetPath("v9.9.10", name),
                           ReleaseServer::Route::Serving(FileOf(name)));
        }
        BootToInstall();
        if (QTest::currentTestFailed()) {
            return;
        }
        RelaunchResult_ = std::unexpected(QString("no way"));
        const auto gate = HoldTrash();
        QTRY_VERIFY_WITH_TIMEOUT(InstallButton()->isVisible(), TIMEOUT_MS);
        ClickItem(InstallButton());
        QTRY_VERIFY_WITH_TIMEOUT(gate->Reached.load(), TIMEOUT_MS);
        QCOMPARE(InstallState(), QString("installing"));

        // The newer release is found by a check made now.
        Server_->Answer(200, ReleaseOf("v9.9.10", {}, names));
        SetCheck(false);
        SetCheck(true);
        QTRY_COMPARE_WITH_TIMEOUT(Available(), QString("9.9.10"), TIMEOUT_MS);
        QCOMPARE(InstallState(), QString("installing"));
        QCOMPARE(StripText(), QString("Installing roost v9.9.9..."));
        QCOMPARE(DownloadState(), QString("ready"));

        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression("roost: roost 9.9.9 is installed, but this "
                               "window cannot start it: no way"));
        gate->Let();
        QTRY_COMPARE_WITH_TIMEOUT(InstallState(), QString("failed"),
                                  TIMEOUT_MS);
        QCOMPARE(Relaunched_.size(), 1);
        QCOMPARE(InstalledMarker(), QByteArray("new"));
        // The package of v9.9.9 is not the one of the offer any more.
        QCOMPARE(DownloadState(), QString());

        // The next click is for v9.9.10: its files are fetched (the signed
        // files of the fixtures are v9.9.9's, so the download ends in the check
        // of the release's name, before the package), the old package is not
        // installed again.
        ExpectFailure("the signed comment is");
        ClickItem(InstallButton());
        QTRY_COMPARE_WITH_TIMEOUT(
            Server_->AskedFor(AssetPath("v9.9.10", "SHA256SUMS")), 1,
            TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!Singleton_->updates()->installBusy(),
                                 TIMEOUT_MS);
        QVERIFY(StripText().contains("roost v9.9.9 SHA256SUMS"));
        QCOMPARE(Relaunched_.size(), 1);
        QCOMPARE(Trashed_.size(), 1);
        QCOMPARE(Quits_, 0);
    }

    // The check is turned off while the install runs: it goes on, and the strip
    // with its text stays until it ends; then the setting is followed (the
    // strip goes), or the window quits.
    void TurningTheCheckOffDuringTheInstallKeepsTheStripUntilItEnds_data() {
        QTest::addColumn<bool>("startFails");
        QTest::newRow("the new version starts") << false;
        QTest::newRow("the new version cannot be started") << true;
    }

    void TurningTheCheckOffDuringTheInstallKeepsTheStripUntilItEnds() {
        QFETCH(bool, startFails);
        ServeRelease();
        BootToInstall();
        if (QTest::currentTestFailed()) {
            return;
        }
        if (startFails) {
            RelaunchResult_ = std::unexpected(QString("no way"));
        }
        const auto gate = HoldTrash();
        QTRY_VERIFY_WITH_TIMEOUT(InstallButton()->isVisible(), TIMEOUT_MS);
        ClickItem(InstallButton());
        QTRY_VERIFY_WITH_TIMEOUT(gate->Reached.load(), TIMEOUT_MS);
        SetCheck(false);
        QTRY_VERIFY_WITH_TIMEOUT(!Singleton_->updates()->enabled(), TIMEOUT_MS);
        // The setting is off, and the install is still shown.
        QVERIFY(StripShown());
        QCOMPARE(InstallState(), QString("installing"));
        QCOMPARE(StripText(), QString("Installing roost v9.9.9..."));
        QVERIFY(!Root()
                     ->findChild<QQuickItem*>("updateDismissButton")
                     ->isEnabled());
        if (startFails) {
            QTest::ignoreMessage(
                QtWarningMsg,
                QRegularExpression("roost: roost 9.9.9 is installed, but this "
                                   "window cannot start it: no way"));
        }
        gate->Let();
        if (startFails) {
            QTRY_COMPARE_WITH_TIMEOUT(InstallState(), QString("failed"),
                                      TIMEOUT_MS);
            // Over: the setting is followed, and the package is dropped.
            QTRY_VERIFY_WITH_TIMEOUT(!StripShown(), TIMEOUT_MS);
            QCOMPARE(DownloadState(), QString());
            QCOMPARE(Quits_, 0);
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(Quits_, 1, TIMEOUT_MS);
            QVERIFY(StripShown());
            QCOMPARE(StripText(), QString("Installing roost v9.9.9..."));
        }
        QCOMPARE(InstalledMarker(), QByteArray("new"));
    }
#endif
};

QTEST_MAIN(UpdatesTest)
#include "updates_test.moc"
