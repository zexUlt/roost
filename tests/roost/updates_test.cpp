// The check for a newer release (design/17-updates.md): the real window, the
// real roostd and the real store; only GitHub is replaced, by a server on the
// loopback that answers what a case says, and ROOST_TEST_UPDATES_URL points the
// check at it. The cases: a newer release is offered, what is not newer or not
// a release is not, a failed request shows nothing and is warned of once,
// settings.updates.check turns the check off, and Dismiss lasts until a newer
// version comes.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUrl>

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
            Heads_.remove(socket);
            if (Hold_) {
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
        socket.write("HTTP/1.1 " + QByteArray::number(Status_) +
                     " Canned\r\nContent-Type: application/json\r\n"
                     "Content-Length: " +
                     QByteArray::number(Body_.size()) +
                     "\r\nConnection: close\r\n\r\n" + Body_);
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

    std::unique_ptr<ReleaseServer> Server_;
    std::optional<ScopedEnv> Url_;

private slots:
    void init() {
        Prepare();
        FailOnWarnings();
    }

    void cleanup() {
        Teardown();
        Url_.reset();
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
};

QTEST_MAIN(UpdatesTest)
#include "updates_test.moc"
