// The banner of the window's shell while the launcher has a roostd coming up:
// "Starting roostd…" and nothing else changes. The launcher here counts the
// starts and starts no process.

#include <QQmlApplicationEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <memory>

#include "api/api.hpp"
#include "helpers/refusing_server.hpp"
#include "qml_api/roost_singleton.hpp"
#include "roost/daemon_launcher.hpp"
#include "roost/plugin_loader.hpp"
#include "roost/shell.hpp"

using namespace roost;
using namespace std::chrono_literals;

Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {
constexpr int TIMEOUT_MS = 10000;
}  // namespace

class ShellStartingTest : public QObject {
    Q_OBJECT

private:
    void Boot(const QString& socket, std::chrono::milliseconds startTimeout) {
        Api_ = std::make_unique<api::Api>(api::ApiOptions{
            .FirstReconnectDelay = 20ms, .MaxReconnectDelay = 40ms});
        Launcher_ = std::make_unique<DaemonLauncher>(
            *Api_, DaemonLauncher::Command{}, startTimeout,
            [this](const DaemonLauncher::Command&) {
                ++Starts_;
                return true;
            });
        Engine_ = std::make_unique<QQmlApplicationEngine>();
        Singleton_ =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        QVERIFY(Singleton_ != nullptr);
        Singleton_->setApi(*Api_);
        Api_->Connect(socket);
        Engine_->loadFromModule("Roost.Shell", "Main");
        QVERIFY(!Engine_->rootObjects().isEmpty());
        Loader_ = std::make_unique<PluginLoader>(*Engine_, Api_->Plugins(),
                                                 *Singleton_);
        Shell_ =
            std::make_unique<Shell>(*Engine_->rootObjects().first(), *Loader_,
                                    *Api_, socket, Launcher_.get());
    }

    QObject* Root() { return Engine_->rootObjects().first(); }
    QString Connection() { return Root()->property("connection").toString(); }
    QString BannerText() {
        return Root()
            ->findChild<QQuickItem*>("connectionText")
            ->property("text")
            .toString();
    }

    QTemporaryDir Dir_;
    std::unique_ptr<api::Api> Api_;
    std::unique_ptr<DaemonLauncher> Launcher_;
    std::unique_ptr<QQmlApplicationEngine> Engine_;
    RoostSingleton* Singleton_ = nullptr;
    std::unique_ptr<PluginLoader> Loader_;
    std::unique_ptr<Shell> Shell_;
    int Starts_ = 0;

private slots:
    void cleanup() {
        // The shell before the loader, the loader before the engine.
        Shell_.reset();
        Loader_.reset();
        Engine_.reset();
        Singleton_ = nullptr;
        Launcher_.reset();
        Api_.reset();
        Starts_ = 0;
    }

    // While the start is in flight the banner says so (and stays visible and
    // unfocused: it is text); when the attempt is over without a connection it
    // goes back to the state of the connection.
    void TheBannerSaysStartingWhileTheStartIsInFlight() {
        Boot(Dir_.filePath("none.sock"), 3s);
        QTRY_COMPARE_WITH_TIMEOUT(Connection(), QString("starting"),
                                  TIMEOUT_MS);
        QCOMPARE(BannerText(), QString("Starting roostd…"));
        QVERIFY(
            Root()->findChild<QQuickItem*>("connectionBanner")->isVisible());
        QVERIFY(!Root()->findChild<QQuickItem*>("retryButton")->isVisible());
        QVERIFY(Starts_ >= 1);
        // The attempt ends unanswered: the banner is the old one again, until
        // the next NoListener starts another (a moment later, so what the
        // banner shows is read when the end is signalled; the shell, connected
        // first, has already reacted).
        QStringList atEnd;
        connect(Launcher_.get(), &DaemonLauncher::StartingChanged, this,
                [this, &atEnd](bool starting) {
                    if (!starting) {
                        atEnd.append(Connection());
                    }
                });
        QTRY_VERIFY_WITH_TIMEOUT(!atEnd.isEmpty(), TIMEOUT_MS);
        QVERIFY2(atEnd.first() != QString("starting"),
                 qPrintable(atEnd.first()));
        QTRY_VERIFY_WITH_TIMEOUT(Starts_ >= 2, TIMEOUT_MS);
    }

    // A refusing server is not "no listener": the banner is the refusal.
    void ARefusalIsNeverStarting() {
        testing::RefusingServer server;
        QVERIFY(server.Listen(Dir_.filePath("old.sock")));
        Boot(Dir_.filePath("old.sock"), 3s);
        QTRY_COMPARE_WITH_TIMEOUT(Connection(), QString("refused"), TIMEOUT_MS);
        QTest::qWait(200);
        QCOMPARE(Connection(), QString("refused"));
        QCOMPARE(Starts_, 0);
    }
};

QTEST_MAIN(ShellStartingTest)
#include "shell_starting_test.moc"
