// The `session` property of TerminalItem, in a QQmlEngine over the real
// roostd with a fixture plugin whose agent attaches to fake_tui: the item
// opens the session it is given, draws it, sends its input and its size, and
// opens it again when the window reconnects. The slow agent answers
// agent.attachCommand late.

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>

#include <atomic>
#include <memory>

#include "api/api.hpp"
#include "helpers/channels_left.hpp"
#include "helpers/roostd_process.hpp"
#include "qml_api/roost_singleton.hpp"
#include "qml_api/terminal_item.hpp"
#include "terminal/renderer.hpp"
#include "terminal/session_pane.hpp"

using namespace roost;
using roost::testing::ChannelsLeft;
using roost::testing::RoostdProcess;

// The static plugin of module Roost.Api, linked into this executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)

namespace {

constexpr int TIMEOUT_MS = 10000;
constexpr int COLUMNS = 80;
constexpr int ROWS = 24;
// How long the slow plugin holds back its answer to agent.attachCommand.
constexpr int SLOW_DELAY_MS = 300;
// Each query answered once; a digit after the 1 would be more than once.
const QRegularExpression ANSWERED_ONCE(
    QStringLiteral(R"(answers da1=1 cpr=1(?!\d))"));
const QRegularExpression ANSWERED_TWICE(
    QStringLiteral(R"(answers da1=2 cpr=2(?!\d))"));

// The item is the page's only child; `ref` is what its session shows.
const char* const QML = R"(
import QtQuick
import Roost.Api

Item {
    property var ref: null
    property alias terminal: terminal
    // What QML reads from `session`: null, not undefined.
    property bool sessionIsNull: terminal.session === null

    TerminalItem {
        id: terminal
        objectName: "terminal"
        anchors.fill: parent
        session: parent.ref
    }
}
)";

// As above, with the session given in the file.
const char* const QML_LITERAL = R"(
import QtQuick
import Roost.Api

Item {
    TerminalItem {
        objectName: "terminal"
        anchors.fill: parent
        session: ({agent: "fast-agent", id: "s1"})
    }
}
)";

QVariantMap Ref(const QString& agent, const QString& id) {
    return {{"agent", agent}, {"id", id}};
}

QString SizeLine(int columns, int rows) {
    return QStringLiteral("size %1x%2").arg(columns).arg(rows);
}

// What the terminal shows, as the VT sequences that rebuild it.
QString Screen(const std::shared_ptr<TerminalSession>& session) {
    if (!session) {
        return {};
    }
    const auto lock = session->Lock();
    return QString::fromUtf8(session->Dump(lock).value_or("dump failed"));
}

}  // namespace

class TerminalItemSessionTest : public QObject {
    Q_OBJECT

private:
    // The plugin of an agent "<id>-agent" with one session, s1, that attaches
    // to fake_tui.
    bool AddAgent(const QString& plugin, int delayMs) {
        if (!Daemon_->AddFixturePlugin(plugin, FIXTURE_BACKEND_PATH)) {
            return false;
        }
        QFile file(Daemon_->PluginsPath() + u'/' + plugin + "/agent.json");
        if (!file.open(QIODevice::ReadWrite)) {
            return false;
        }
        QJsonObject answers = QJsonDocument::fromJson(file.readAll()).object();
        QJsonObject attach{{"result", QJsonObject{{"program", FAKE_TUI_PATH}}}};
        if (delayMs > 0) {
            attach.insert("delayMs", delayMs);
        }
        answers.insert("agent.attachCommand",
                       QJsonObject{{plugin + "-agent/s1", attach}});
        file.resize(0);
        return file.write(QJsonDocument(answers).toJson()) > 0;
    }

    // The engine with the singleton, and the Api in it unless `withApi` is
    // false. Not connected yet.
    bool PrepareEngine(bool withApi = true) {
        Engine_ = std::make_unique<QQmlEngine>();
        auto* const singleton =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        if (singleton == nullptr) {
            return false;
        }
        if (withApi) {
            // Short retry delays: a restart of roostd under load must not push
            // the next attempt past the test's timeout.
            Api_ = std::make_unique<api::Api>(api::ApiOptions{
                .FirstReconnectDelay = std::chrono::milliseconds{20},
                .MaxReconnectDelay = std::chrono::milliseconds{80}});
            singleton->setApi(*Api_);
        }
        return true;
    }

    // The daemon, the Api connected to it, the engine and a shown window of
    // exactly COLUMNS x ROWS cells.
    bool Prepare() {
        if (!Daemon_->IsValid() || !AddAgent("fast", 0) ||
            !AddAgent("slow", SLOW_DELAY_MS) || !Daemon_->Start(ROOSTD_PATH) ||
            !PrepareEngine()) {
            return false;
        }
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        Api_->Connect(Daemon_->SocketPath());
        return connected.wait(TIMEOUT_MS) || Api_->IsConnected();
    }

    // The page in the window, sized to a grid unless `sized` is false; then
    // the window is not shown either, and ShowWindow does it after ResizeGrid.
    // Qt Quick frames are counted as drawn when the item rebuilt a row in them.
    bool ShowPage(const char* qml, bool sized = true) {
        Window_ = std::make_unique<QQuickWindow>();
        QQmlComponent component(Engine_.get());
        component.setData(qml, QUrl());
        Page_.reset(qobject_cast<QQuickItem*>(component.create()));
        if (!Page_) {
            qWarning("%s", qPrintable(component.errorString()));
            return false;
        }
        Item_ = Page_->findChild<TerminalItem*>("terminal");
        if (Item_ == nullptr) {
            return false;
        }
        Page_->setParentItem(Window_->contentItem());
        // In the render thread, right after the sync that rebuilt the rows,
        // while the GUI thread waits: the item may be read then. The pointer
        // is copied; the window is gone before the item.
        connect(
            Window_.get(), &QQuickWindow::afterSynchronizing, Window_.get(),
            [this, item = Item_] {
                if (item->rowsRebuilt() > 0) {
                    ++DrawnFrames_;
                }
            },
            Qt::DirectConnection);
        if (!sized) {
            return true;
        }
        ResizeGrid(COLUMNS, ROWS);
        return ShowWindow();
    }

    bool ShowWindow() {
        Window_->show();
        return QTest::qWaitForWindowExposed(Window_.get()) &&
               QTest::qWaitForWindowActive(Window_.get());
    }

    // The window goes first: its render thread reads the item in each frame.
    void DropPage() {
        if (Page_) {
            Page_->setParentItem(nullptr);
        }
        Window_.reset();
        Item_ = nullptr;
        Page_.reset();
    }

    // The window and the page, to a grid of the given size.
    void ResizeGrid(int columns, int rows) {
        const CellMetrics cell = MeasureCell(Item_->font());
        Window_->resize(columns * cell.Width, rows * cell.Height);
        Page_->setSize(QSizeF(Window_->size()));
        QCOMPARE(Item_->columns(), columns);
        QCOMPARE(Item_->rows(), rows);
    }

    // The session that the item's pane shows, for its screen.
    std::shared_ptr<TerminalSession> SessionOf() const {
        const auto* const pane = Item_->findChild<SessionPane*>();
        return pane != nullptr ? pane->Session() : nullptr;
    }

    // Waits until the item shows the session and the TUI has had its queries
    // answered: fake_tui reacts to a key only if it arrives in a read of its
    // own.
    void SettleOpen() {
        QTRY_COMPARE_WITH_TIMEOUT(Item_->status(), QStringLiteral("open"),
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Screen(SessionOf()).contains(ANSWERED_ONCE),
                                 TIMEOUT_MS);
    }

    std::unique_ptr<RoostdProcess> Daemon_;
    std::unique_ptr<api::Api> Api_;
    std::unique_ptr<QQmlEngine> Engine_;
    std::unique_ptr<QQuickWindow> Window_;
    std::unique_ptr<QQuickItem> Page_;
    TerminalItem* Item_ = nullptr;
    std::atomic<int> DrawnFrames_ = 0;

private slots:
    void init() {
        Daemon_ = std::make_unique<RoostdProcess>();
        DrawnFrames_ = 0;
    }

    void cleanup() {
        // The page and the engine go first, as in main(): the Api outlives
        // them.
        DropPage();
        Engine_.reset();
        Api_.reset();
        Daemon_.reset();
    }

    void ASessionGivenInQmlOpensAndIsDrawn() {
        QVERIFY(Prepare());
        QVERIFY(ShowPage(QML_LITERAL));
        QCOMPARE(Item_->sessionRef(), QVariant(Ref("fast-agent", "s1")));

        QTRY_COMPARE_WITH_TIMEOUT(Item_->status(), QStringLiteral("open"),
                                  TIMEOUT_MS);
        QVERIFY(Item_->statusMessage().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(Screen(SessionOf()).contains("fake tui"),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(DrawnFrames_ > 0, TIMEOUT_MS);
    }

    // The item has no size when it is created from QML; the session opens
    // once it has one, and the TUI never sees another.
    void ASessionGivenBeforeTheItemHasASizeOpensAtTheRealSize() {
        QVERIFY(Prepare());
        QVERIFY(ShowPage(QML_LITERAL, false));
        auto* const pane = Item_->findChild<SessionPane*>();
        QVERIFY(pane != nullptr);
        QObject scope;
        QString firstSize;
        connect(pane, &SessionPane::Updated, &scope, [&] {
            const auto match =
                QRegularExpression(QStringLiteral(R"(size \d+x\d+)"))
                    .match(Screen(pane->Session()));
            if (firstSize.isEmpty() && match.hasMatch()) {
                firstSize = match.captured();
            }
        });
        QCOMPARE(Item_->status(), QStringLiteral("none"));
        QCOMPARE(Item_->sessionRef(), QVariant(Ref("fast-agent", "s1")));

        ResizeGrid(COLUMNS, ROWS);
        QVERIFY(ShowWindow());

        QTRY_COMPARE_WITH_TIMEOUT(Item_->status(), QStringLiteral("open"),
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!firstSize.isEmpty(), TIMEOUT_MS);
        QCOMPARE(firstSize, SizeLine(COLUMNS, ROWS));
    }

    void SessionReadsNullInQmlUntilThereIsARef() {
        QVERIFY(Prepare());
        QVERIFY(ShowPage(QML));
        QVERIFY(Item_->sessionRef().isNull());
        QVERIFY(Page_->property("sessionIsNull").toBool());

        Page_->setProperty("ref", Ref("fast-agent", "s1"));

        QVERIFY(!Page_->property("sessionIsNull").toBool());

        // An invalid value reads back as null as well.
        Page_->setProperty("ref", QStringLiteral("fast-agent/s1"));

        QCOMPARE(Item_->status(), QStringLiteral("failed"));
        QVERIFY(Item_->sessionRef().isNull());
        QVERIFY(Page_->property("sessionIsNull").toBool());
    }

    void TheSameRefAgainDoesNotReopenAnEndedSession() {
        QVERIFY(Prepare());
        QVERIFY(ShowPage(QML));
        Page_->setProperty("ref", Ref("fast-agent", "s1"));
        SettleOpen();
        Item_->forceActiveFocus();
        QVERIFY(QTest::qWaitFor([&] { return Item_->hasActiveFocus(); }));
        QTest::keyClick(Window_.get(), 'x');
        QTRY_COMPARE_WITH_TIMEOUT(Item_->status(), QStringLiteral("ended"),
                                  TIMEOUT_MS);
        const auto session = SessionOf();
        QSignalSpy refChanged(Item_, &TerminalItem::sessionRefChanged);

        Item_->setSessionRef(Ref("fast-agent", "s1"));

        QCOMPARE(Item_->status(), QStringLiteral("ended"));
        QCOMPARE(SessionOf(), session);
        QCOMPARE(refChanged.count(), 0);
        // Only reopen() opens it again.
        QMetaObject::invokeMethod(Item_, "reopen");
        QCOMPARE(Item_->status(), QStringLiteral("opening"));
    }

    // Not through "none" on the way: a binding on the status would show it.
    void AnInvalidRefGoesStraightToFailed() {
        QVERIFY(Prepare());
        QVERIFY(ShowPage(QML));
        Page_->setProperty("ref", Ref("fast-agent", "s1"));
        SettleOpen();
        QObject scope;
        QStringList statuses;
        connect(Item_, &TerminalItem::statusChanged, &scope,
                [&] { statuses.push_back(Item_->status()); });

        Page_->setProperty("ref", QStringLiteral("fast-agent/s1"));

        QCOMPARE(statuses, QStringList{QStringLiteral("failed")});
        QVERIFY(!SessionOf());
    }

    void TheStatusFollowsTheSessionThroughOpeningToNone() {
        QVERIFY(Prepare());
        QVERIFY(ShowPage(QML));
        QCOMPARE(Item_->status(), QStringLiteral("none"));
        QSignalSpy statusChanged(Item_, &TerminalItem::statusChanged);

        Page_->setProperty("ref", Ref("fast-agent", "s1"));

        QCOMPARE(Item_->status(), QStringLiteral("opening"));
        SettleOpen();

        Page_->setProperty("ref", QVariant::fromValue(nullptr));

        QCOMPARE(Item_->status(), QStringLiteral("none"));
        QVERIFY(!SessionOf());
        QVERIFY(statusChanged.count() >= 3);
    }

    void TypedKeysReachTheTui() {
        QVERIFY(Prepare());
        QVERIFY(ShowPage(QML));
        Page_->setProperty("ref", Ref("fast-agent", "s1"));
        SettleOpen();
        Item_->forceActiveFocus();
        QVERIFY(QTest::qWaitFor([&] { return Item_->hasActiveFocus(); }));

        // 'q' makes fake_tui ask its terminal again.
        QTest::keyClick(Window_.get(), 'q');

        QTRY_VERIFY_WITH_TIMEOUT(Screen(SessionOf()).contains(ANSWERED_TWICE),
                                 TIMEOUT_MS);
    }

    void AResizeOfTheItemReachesThePty() {
        QVERIFY(Prepare());
        QVERIFY(ShowPage(QML));
        Page_->setProperty("ref", Ref("fast-agent", "s1"));
        SettleOpen();
        QVERIFY(Screen(SessionOf()).contains(SizeLine(COLUMNS, ROWS)));

        ResizeGrid(100, 30);

        QTRY_VERIFY_WITH_TIMEOUT(
            Screen(SessionOf()).contains(SizeLine(100, 30)), TIMEOUT_MS);
    }

    void ATuiThatExitsEndsTheStatusAndEnterReopensWithReopen() {
        QVERIFY(Prepare());
        QVERIFY(ShowPage(QML));
        Page_->setProperty("ref", Ref("fast-agent", "s1"));
        SettleOpen();
        Item_->forceActiveFocus();
        QVERIFY(QTest::qWaitFor([&] { return Item_->hasActiveFocus(); }));

        QTest::keyClick(Window_.get(), 'x');

        QTRY_COMPARE_WITH_TIMEOUT(Item_->status(), QStringLiteral("ended"),
                                  TIMEOUT_MS);
        QCOMPARE(Item_->statusMessage(), QStringLiteral("exited with code 0"));

        QMetaObject::invokeMethod(Item_, "reopen");

        QCOMPARE(Item_->status(), QStringLiteral("opening"));
        QTRY_COMPARE_WITH_TIMEOUT(Item_->status(), QStringLiteral("open"),
                                  TIMEOUT_MS);
    }

    void AnUnknownSessionFails() {
        QVERIFY(Prepare());
        QVERIFY(ShowPage(QML));

        Page_->setProperty("ref", Ref("nope", "s1"));

        QTRY_COMPARE_WITH_TIMEOUT(Item_->status(), QStringLiteral("failed"),
                                  TIMEOUT_MS);
        QVERIFY(!Item_->statusMessage().isEmpty());
    }

    void SomethingThatIsNoSessionRefFails() {
        QVERIFY(Prepare());
        QVERIFY(ShowPage(QML));

        for (const QVariant& value :
             {QVariant(QVariantMap{{"agent", "fast-agent"}}),
              QVariant(QVariantMap{{"agent", "fast-agent"}, {"id", ""}}),
              QVariant(QVariantMap{{"agent", 1}, {"id", "s1"}}),
              QVariant(QStringLiteral("fast-agent/s1"))}) {
            Page_->setProperty("ref", QVariant::fromValue(nullptr));
            QCOMPARE(Item_->status(), QStringLiteral("none"));

            Page_->setProperty("ref", value);

            QCOMPARE(Item_->status(), QStringLiteral("failed"));
            QCOMPARE(Item_->statusMessage(),
                     QStringLiteral("not a SessionRef"));
            QVERIFY(Item_->sessionRef().isNull());
        }
    }

    // The daemon goes: the item says so and keeps the screen; it comes back
    // and the item opens the same session again, by itself.
    void ALostConnectionReopensTheSameSessionWhenBack() {
        QVERIFY(Prepare());
        QVERIFY(ShowPage(QML));
        Page_->setProperty("ref", Ref("fast-agent", "s1"));
        SettleOpen();
        const auto session = SessionOf();

        QVERIFY(Daemon_->Stop());

        QTRY_COMPARE_WITH_TIMEOUT(Item_->status(),
                                  QStringLiteral("disconnected"), TIMEOUT_MS);
        QCOMPARE(SessionOf(), session);
        QVERIFY(Screen(session).contains("fake tui"));

        QVERIFY(Daemon_->Start(ROOSTD_PATH));

        QTRY_COMPARE_WITH_TIMEOUT(Item_->status(), QStringLiteral("open"),
                                  TIMEOUT_MS);
        QCOMPARE(Item_->sessionRef(), QVariant(Ref("fast-agent", "s1")));
        QVERIFY(SessionOf() != session);
        QTRY_VERIFY_WITH_TIMEOUT(Screen(SessionOf()).contains("fake tui"),
                                 TIMEOUT_MS);
    }

    void WithoutAnApiTheStatusIsFailedAndNothingCrashes() {
        QVERIFY(PrepareEngine(false));
        QVERIFY(ShowPage(QML));

        Page_->setProperty("ref", Ref("fast-agent", "s1"));

        QCOMPARE(Item_->status(), QStringLiteral("failed"));
        QCOMPARE(Item_->statusMessage(),
                 QStringLiteral("no roostd API in this window"));
        QMetaObject::invokeMethod(Item_, "reopen");
        QCOMPARE(Item_->status(), QStringLiteral("failed"));
        Page_->setProperty("ref", QVariant::fromValue(nullptr));
        QCOMPARE(Item_->status(), QStringLiteral("none"));
    }

    void ACreatedItemWithoutAnEngineFailsToo() {
        TerminalItem item;
        item.setSessionRef(Ref("fast-agent", "s1"));
        QCOMPARE(item.status(), QStringLiteral("failed"));
        QCOMPARE(item.statusMessage(),
                 QStringLiteral("no roostd API in this window"));
    }

    // The answer finds no item: the session completes into the window's warm
    // terminals, and nothing of the item is touched.
    void AnItemDestroyedWhileOpeningLeavesNoCrash() {
        QVERIFY(Prepare());
        QVERIFY(ShowPage(QML));
        Page_->setProperty("ref", Ref("slow-agent", "s1"));
        QCOMPARE(Item_->status(), QStringLiteral("opening"));

        DropPage();

        // The item's open was asked first, so its answer has been dealt with
        // when this one is answered; the window still holds its channel, as a
        // warm entry that no item shows.
        const SessionKey slow{"slow-agent", "s1"};
        QTRY_COMPARE_WITH_TIMEOUT(Api_->Terminals().Status(slow),
                                  EPaneStatus::Open, TIMEOUT_MS);
        QVERIFY(!Api_->Terminals().IsShown(slow));
        QCOMPARE(ChannelsLeft(Api_->Io(), "slow-agent", "s1", TIMEOUT_MS), 1);

        // The window still serves a new item.
        QVERIFY(ShowPage(QML));
        Page_->setProperty("ref", Ref("fast-agent", "s1"));
        QTRY_COMPARE_WITH_TIMEOUT(Item_->status(), QStringLiteral("open"),
                                  TIMEOUT_MS);
    }
};

QTEST_MAIN(TerminalItemSessionTest)
#include "terminal_item_session_test.moc"
