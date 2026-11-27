// The desktop notifications of the window (src/roost/window_notifications.hpp):
// the real roostd with the real notifications plugin and a fixture agent whose
// sessions change state, and windows of the real controller, each on a
// connection of its own, with a notifier that records what it is asked to show
// instead of the operating system. What is checked is the decision of the
// window: whether it shows and what, and which of two windows does; the
// notification of the system itself is checked by hand.

#include <QEventLoop>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWindow>

#include <memory>
#include <vector>

#include "api/api.hpp"
#include "core/rpc_client.hpp"
#include "helpers/recording_notifier.hpp"
#include "helpers/roostd_process.hpp"
#include "qml_api/terminal_item.hpp"
#include "roost/desktop_notifier.hpp"
#include "roost/window_notifications.hpp"

using namespace roost;
using roost::testing::RecordingNotifier;
using roost::testing::RoostdProcess;
using roost::testing::Shown;

namespace {

constexpr int TIMEOUT_MS = 10000;
const QString AGENT = QStringLiteral("fix-agent");
const QString PLUGIN = QStringLiteral("notifications");

bool WaitFor(const QFuture<rpc::Result>& future) {
    if (future.isFinished()) {
        return true;
    }
    QEventLoop loop;
    QFutureWatcher<rpc::Result> watcher;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop,
                     &QEventLoop::quit);
    QTimer::singleShot(TIMEOUT_MS, &loop, &QEventLoop::quit);
    watcher.setFuture(future);
    loop.exec();
    return future.isFinished();
}

QString MessageOf(const rpc::Result& result) {
    return result ? QString() : result.error().Message;
}

// What the fixture backend sends roostd as the change of a session.
QJsonObject Report(const QString& id, const QString& state,
                   const QString& title = {}) {
    return {
        {"jsonrpc", "2.0"},
        {"method", "session.stateChanged"},
        {"params", QJsonObject{{"agent", AGENT},
                               {"session", QJsonObject{{"id", id},
                                                       {"title", title},
                                                       {"cwd", ""},
                                                       {"state", state}}}}}};
}

// One window of the real code: a connection of its own, a notifier that
// records, and a plain window (the platform of the tests is offscreen) that it
// reacts to the focus of (a Quick window, so that it can hold a terminal).
struct Window {
    explicit Window(const QString& socket)
        : Api(std::make_unique<api::Api>()),
          Notifier(std::make_unique<RecordingNotifier>()),
          Surface(std::make_unique<QQuickWindow>()),
          Notifications(std::make_unique<WindowNotifications>(*Api, *Notifier,
                                                              *Surface)) {
        Surface->resize(200, 100);
        Api->Connect(socket);
    }

    ~Window() {
        Notifications.reset();
        Other.reset();
        Surface.reset();
        Notifier.reset();
        Api.reset();
    }

    // Shows the window and gives it the focus; what it tells the others is
    // seen by `Notifications` through the bus.
    bool Focus() {
        Surface->show();
        return QTest::qWaitForWindowExposed(Surface.get()) &&
               (Surface->requestActivate(),
                QTest::qWaitForWindowActive(Surface.get()));
    }

    // The window shows the session in a terminal, as the terminal panel does.
    void ShowSession(const QString& id) {
        auto* terminal = new TerminalItem(Surface->contentItem());
        terminal->setParentItem(Surface->contentItem());
        terminal->setSize(QSizeF(100, 50));
        terminal->setProperty("session",
                              QVariantMap{{"agent", AGENT}, {"id", id}});
        Terminal = terminal;
    }

    // The focus goes to a window that is not this one (the settings window,
    // another application): this one is not the active window.
    bool LoseFocus() {
        Other = std::make_unique<QWindow>();
        Other->resize(100, 100);
        Other->show();
        return QTest::qWaitForWindowExposed(Other.get()) &&
               (Other->requestActivate(),
                QTest::qWaitForWindowActive(Other.get())) &&
               !Surface->isActive();
    }

    // The titles of what was shown, in order.
    QStringList Titles() const {
        QStringList titles;
        for (const Shown& shown : Notifier->Calls) {
            titles.append(shown.Title);
        }
        return titles;
    }

    std::unique_ptr<api::Api> Api;
    std::unique_ptr<RecordingNotifier> Notifier;
    std::unique_ptr<QQuickWindow> Surface;
    std::unique_ptr<WindowNotifications> Notifications;
    std::unique_ptr<QWindow> Other;
    QPointer<TerminalItem> Terminal;
};

}  // namespace

class WindowNotificationsTest : public QObject {
    Q_OBJECT

private:
    // The real roostd with the real plugin and the fixture agent.
    bool Start() {
        Roostd_ = std::make_unique<RoostdProcess>();
        if (!Roostd_->IsValid() ||
            !Roostd_->AddFixturePlugin("fix", FIXTURE_BACKEND_PATH) ||
            !QFile::link(NOTIFICATIONS_PLUGIN_DIR,
                         Roostd_->PluginsPath() + "/notifications") ||
            !Roostd_->Start(ROOSTD_PATH)) {
            return false;
        }
        return Handshake();
    }

    // The test's own client of the running roostd, once the plugin answers.
    bool Handshake() {
        Client_ = std::make_unique<RpcClient>();
        const QFuture<rpc::Result> hello =
            Client_->Connect(Roostd_->SocketPath());
        if (!WaitFor(hello) || !hello.result()) {
            return false;
        }
        // roostd has read what the backend asked for at its start, in order,
        // before it answers anything else the backend says: from now on the
        // backend is subscribed, and a state it is told is seen.
        bool answered = false;
        for (int i = 0; i < 100 && !answered; ++i) {
            answered = Test().has_value();
            if (!answered) {
                QTest::qWait(50);
            }
        }
        return answered;
    }

    rpc::Result Call(const QString& method, const QJsonValue& params) {
        const QFuture<rpc::Result> future = Client_->Request(method, params);
        if (!WaitFor(future)) {
            return std::unexpected(rpc::Error{.Message = "timed out"});
        }
        return future.result();
    }

    // notifications.test: the plugin publishes a notification with no session.
    rpc::Result Test() {
        return Call("commands.run",
                    QJsonObject{{"command", "notifications.test"}});
    }

    // Has the fixture backend report these, in order. roostd has published
    // them when this returns.
    void Reports(const QJsonArray& messages) {
        const rpc::Result sent = Call(
            "commands.run",
            QJsonObject{{"command", "fixture.notify"}, {"args", messages}});
        QVERIFY2(sent, qPrintable(MessageOf(sent)));
    }

    // Replaces `settings`, as the settings page does.
    void PutSettings(const QJsonObject& settings) {
        const rpc::Result patched =
            Call("store.patch",
                 QJsonObject{{"plugin", PLUGIN},
                             {"patch", QJsonObject{{"settings", settings}}}});
        QVERIFY2(patched, qPrintable(MessageOf(patched)));
    }

    // A window whose connection is up and which has the focus, and which has
    // heard that it has (the others, too, when they are open: they hear the
    // same event).
    std::unique_ptr<Window> OpenFocused() {
        auto window = std::make_unique<Window>(Roostd_->SocketPath());
        [&] {
            QTRY_VERIFY_WITH_TIMEOUT(window->Api->IsConnected(), TIMEOUT_MS);
            QVERIFY(window->Focus());
            QTRY_VERIFY_WITH_TIMEOUT(window->Notifications->IsLastFocused(),
                                     TIMEOUT_MS);
        }();
        return QTest::currentTestFailed() ? nullptr : std::move(window);
    }

    std::unique_ptr<RoostdProcess> Roostd_;
    std::unique_ptr<RpcClient> Client_;

private slots:
    void cleanup() {
        Client_.reset();
        Roostd_.reset();
    }

    void ABlockedSessionIsShownOnceWithItsTitleAndWhatItNeeds() {
        QVERIFY(Start());
        const auto window = OpenFocused();
        QVERIFY(window);
        Reports({Report("s1", "working", "Fix the build"),
                 Report("s1", "blocked", "Fix the build"),
                 Report("s1", "blocked", "Fix the build")});
        // A session with no title is shown by its id; the failed one after it
        // is the barrier, nothing is shown later than it.
        Reports({Report("s2", "working"), Report("s2", "failed")});
        QTRY_COMPARE_WITH_TIMEOUT(window->Notifier->Calls.size(), size_t{2},
                                  TIMEOUT_MS);
        QCOMPARE(window->Notifier->Calls[0].Title, "Fix the build");
        QCOMPARE(window->Notifier->Calls[0].Body, "needs you");
        QCOMPARE(window->Notifier->Calls[1].Title, "s2");
        QCOMPARE(window->Notifier->Calls[1].Body, "failed");
        QVERIFY(window->Notifier->Calls[0].Id != window->Notifier->Calls[1].Id);
    }

    void NothingIsShownWithDesktopOff() {
        QVERIFY(Start());
        const auto window = OpenFocused();
        QVERIFY(window);
        PutSettings({{"desktop", false}});
        Reports({Report("off", "working"), Report("off", "blocked", "Off")});
        PutSettings({{"desktop", true}});
        // Shown after the one above: had that been shown, it is first.
        Reports({Report("on", "working"), Report("on", "blocked", "On")});
        QTRY_VERIFY_WITH_TIMEOUT(!window->Notifier->Calls.empty(), TIMEOUT_MS);
        QCOMPARE(window->Titles(), (QStringList{"On"}));
    }

    void NothingIsShownForAnEventOutsideTheEventsOfTheSettings() {
        QVERIFY(Start());
        const auto window = OpenFocused();
        QVERIFY(window);
        PutSettings({{"events", QJsonArray{"failed"}}});
        Reports({Report("b", "working"), Report("b", "blocked", "Blocked"),
                 Report("f", "working"), Report("f", "failed", "Failed")});
        QTRY_VERIFY_WITH_TIMEOUT(!window->Notifier->Calls.empty(), TIMEOUT_MS);
        QCOMPARE(window->Titles(), (QStringList{"Failed"}));
    }

    void TheTestOfThePluginIsShownWithNoSession() {
        QVERIFY(Start());
        const auto window = OpenFocused();
        QVERIFY(window);
        const rpc::Result ran = Test();
        QVERIFY2(ran, qPrintable(MessageOf(ran)));
        QTRY_COMPARE_WITH_TIMEOUT(window->Notifier->Calls.size(), size_t{1},
                                  TIMEOUT_MS);
        QCOMPARE(window->Notifier->Calls[0].Body,
                 "Test notification from roost");
    }

    void OnlyTheWindowThatWasFocusedLastShowsAndTheNextOneTakesOver() {
        QVERIFY(Start());
        // The second window is up before the first one is focused: the bus
        // gives an event to those who are subscribed when it is published.
        const auto second = std::make_unique<Window>(Roostd_->SocketPath());
        QTRY_VERIFY_WITH_TIMEOUT(second->Api->IsConnected(), TIMEOUT_MS);
        const auto first = OpenFocused();
        QVERIFY(first);
        // The second window hears of the first one's focus without having had
        // its own: it shows nothing.
        QTRY_COMPARE_WITH_TIMEOUT(second->Notifications->LastFocusedWindow(),
                                  first->Notifications->WindowId(), TIMEOUT_MS);
        QVERIFY(!second->Notifications->IsLastFocused());
        Reports({Report("a", "working"), Report("a", "blocked", "First")});
        QTRY_COMPARE_WITH_TIMEOUT(first->Notifier->Calls.size(), size_t{1},
                                  TIMEOUT_MS);

        // The second one is focused: its connection has had the first
        // notification before it hears of the focus, so that it is known not
        // to have been shown there.
        QVERIFY(second->Focus());
        QTRY_VERIFY_WITH_TIMEOUT(second->Notifications->IsLastFocused(),
                                 TIMEOUT_MS);
        QVERIFY(second->Notifier->Calls.empty());
        QTRY_VERIFY_WITH_TIMEOUT(!first->Notifications->IsLastFocused(),
                                 TIMEOUT_MS);
        Reports({Report("b", "working"), Report("b", "blocked", "Second")});
        QTRY_COMPARE_WITH_TIMEOUT(second->Notifier->Calls.size(), size_t{1},
                                  TIMEOUT_MS);
        QCOMPARE(second->Titles(), (QStringList{"Second"}));

        // And back: the first one has had the second notification before it
        // hears of its own focus.
        QVERIFY(first->Focus());
        QTRY_VERIFY_WITH_TIMEOUT(first->Notifications->IsLastFocused(),
                                 TIMEOUT_MS);
        QCOMPARE(first->Titles(), (QStringList{"First"}));
        QCOMPARE(second->Titles(), (QStringList{"Second"}));
    }

    void ALastFocusedWindowThatLostTheFocusShowsAfterTheConnectionIsBack() {
        QVERIFY(Start());
        const auto window = OpenFocused();
        QVERIFY(window);
        // Another window has the focus; this one is still the last focused.
        QVERIFY(window->LoseFocus());
        QVERIFY(window->Notifications->IsLastFocused());

        QVERIFY(Roostd_->Stop());
        QTRY_VERIFY_WITH_TIMEOUT(!window->Api->IsConnected(), TIMEOUT_MS);
        Client_.reset();
        QVERIFY(Roostd_->Start(ROOSTD_PATH));
        QTRY_VERIFY_WITH_TIMEOUT(window->Api->IsConnected(), TIMEOUT_MS);
        // It has said so again, and heard of it.
        QTRY_VERIFY_WITH_TIMEOUT(window->Notifications->IsLastFocused(),
                                 TIMEOUT_MS);
        QVERIFY(Handshake());
        Reports({Report("r", "working"), Report("r", "blocked", "After")});
        // (The handshake may have shown the test notification of the plugin.)
        QTRY_VERIFY_WITH_TIMEOUT(window->Titles().contains("After"),
                                 TIMEOUT_MS);
    }

    void NothingIsShownForTheSessionThatTheFocusedWindowShows() {
        QVERIFY(Start());
        const auto window = OpenFocused();
        QVERIFY(window);
        window->ShowSession("seen");
        Reports({Report("seen", "working", "Seen"),
                 Report("seen", "blocked", "Seen"),
                 Report("other", "working", "Other"),
                 Report("other", "blocked", "Other")});
        // The one that is not shown is the barrier: it comes after the first.
        QTRY_VERIFY_WITH_TIMEOUT(!window->Notifier->Calls.empty(), TIMEOUT_MS);
        QCOMPARE(window->Titles(), (QStringList{"Other"}));
    }

    void TheSessionThatAWindowOutOfFocusShowsIsShown() {
        QVERIFY(Start());
        const auto window = OpenFocused();
        QVERIFY(window);
        window->ShowSession("seen");
        QVERIFY(window->LoseFocus());
        QVERIFY(window->Notifications->IsLastFocused());
        Reports({Report("seen", "working", "Seen"),
                 Report("seen", "blocked", "Seen")});
        QTRY_COMPARE_WITH_TIMEOUT(window->Notifier->Calls.size(), size_t{1},
                                  TIMEOUT_MS);
        QCOMPARE(window->Titles(), (QStringList{"Seen"}));
    }

    void AWindowThatHeardOfNoFocusShowsNothing() {
        QVERIFY(Start());
        Window unfocused(Roostd_->SocketPath());
        QTRY_VERIFY_WITH_TIMEOUT(unfocused.Api->IsConnected(), TIMEOUT_MS);
        QVERIFY(unfocused.Notifications->LastFocusedWindow().isEmpty());
        Reports({Report("a", "working"), Report("a", "blocked", "Unseen")});
        // The event for the second window is delivered after the first one on
        // the connection of `unfocused`; once it has heard of the focus of
        // another window it has had the notification before it.
        const auto other = OpenFocused();
        QVERIFY(other);
        QTRY_COMPARE_WITH_TIMEOUT(unfocused.Notifications->LastFocusedWindow(),
                                  other->Notifications->WindowId(), TIMEOUT_MS);
        QVERIFY(unfocused.Notifier->Calls.empty());
        QVERIFY(other->Notifier->Calls.empty());
    }
};

QTEST_MAIN(WindowNotificationsTest)
#include "window_notifications_test.moc"
