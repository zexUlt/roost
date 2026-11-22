// A tab the user picks by a click: the panel on it takes the keyboard (the
// layout commands and the keys act on it, not on the panel that is hidden now)
// and the choice is stored as `active` of the stack, so that the next start
// shows the same tab. In the real window against a real roostd.

#include <QDir>
#include <QSignalSpy>
#include <QTest>
#include <algorithm>
#include <optional>

#include "helpers/layout_window.hpp"
#include "helpers/scoped_env.hpp"

using namespace roost;
using namespace roost::testing;

class LayoutTabsChoiceTest : public LayoutWindow {
    Q_OBJECT

private:
    std::optional<ScopedEnv> Home_;
    std::optional<ScopedEnv> Socket_;
    std::optional<ScopedEnv> Config_;

    static int Writes(const QSignalSpy& spy) {
        int count = 0;
        for (const auto& call : spy) {
            if (call.at(0).toString() == QLatin1String("layout")) {
                ++count;
            }
        }
        return count;
    }

    // A right click in the middle of the panel opens its menu; a left click
    // on the item runs it. One write of the layout follows.
    void ClickInMenu(const QString& instance, const char* item) {
        QQuickItem* slot = SlotByInstance(instance);
        QVERIFY(slot != nullptr);
        QVERIFY(slot->isVisible());
        QTest::mouseClick(
            Window(), Qt::RightButton, Qt::NoModifier,
            slot->mapToScene(QPointF(slot->width() / 2, slot->height() / 2))
                .toPoint());
        QObject* popup = Root()->findChild<QObject*>("panelMenu");
        QVERIFY(popup != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(popup->property("visible").toBool() &&
                                     popup->property("instance") == instance,
                                 TIMEOUT_MS);
        QQuickItem* entry = Root()->findChild<QQuickItem*>(item);
        QVERIFY(entry != nullptr);
        QVERIFY(entry->isEnabled());
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        QTest::mouseClick(
            Window(), Qt::LeftButton, Qt::NoModifier,
            entry->mapToScene(QPointF(entry->width() / 2, entry->height() / 2))
                .toPoint());
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!popup->property("visible").toBool(),
                                 TIMEOUT_MS);
    }

    static void CollectByClass(QQuickItem* item, const char* name,
                               QList<QQuickItem*>& out) {
        for (QQuickItem* child : item->childItems()) {
            const QString cls = child->metaObject()->className();
            // "TabBar" must not match "TabButton" and the other way round.
            const auto len = static_cast<qsizetype>(qstrlen(name));
            if (cls.startsWith(name) &&
                (cls.size() == len || cls.at(len) == '_')) {
                out.append(child);
            }
            CollectByClass(child, name, out);
        }
    }

    static void CollectButtons(QQuickItem* item, QList<QQuickItem*>& out) {
        CollectByClass(item, "TabButton", out);
    }

    // The tab buttons of the window, left to right.
    QList<QQuickItem*> TabButtons() {
        QList<QQuickItem*> buttons;
        CollectButtons(Window()->contentItem(), buttons);
        std::sort(buttons.begin(), buttons.end(),
                  [](QQuickItem* a, QQuickItem* b) {
                      return a->mapToScene(QPointF(0, 0)).x() <
                             b->mapToScene(QPointF(0, 0)).x();
                  });
        return buttons;
    }

    // A click on the button of the tab, as the user does it.
    void ClickTab(int index) {
        QTRY_VERIFY_WITH_TIMEOUT(TabButtons().size() > index, TIMEOUT_MS);
        QQuickItem* button = TabButtons().at(index);
        QTest::mouseClick(
            Window(), Qt::LeftButton, Qt::NoModifier,
            button
                ->mapToScene(QPointF(button->width() / 2, button->height() / 2))
                .toPoint());
    }

    int StoredActive() {
        return StoredLayout().value("root").toObject().value("active").toInt(
            -1);
    }

    void BootDefault() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
    }

    // The terminal into tabs with the list: "list | terminal", the terminal
    // shown and focused.
    void TerminalIntoTabs() {
        BootDefault();
        if (QTest::currentTestFailed()) {
            return;
        }
        ClickInMenu("terminal", "menuIntoTabs");
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Shell_->CurrentLayout().Root.Kind,
                                  ELayoutNodeKind::Tabs, TIMEOUT_MS);
        QCOMPARE(Shell_->CurrentLayout().Root.Active, 1);
        QTRY_VERIFY_WITH_TIMEOUT(
            SlotByInstance("terminal")->isVisible() &&
                !SlotByInstance("list")->isVisible() &&
                SlotByInstance("terminal")->hasActiveFocus(),
            TIMEOUT_MS);
        QCOMPARE(StoredActive(), 1);
    }

    // Moves the terminal-2 and then the list into the tabs of the terminals:
    // "list | terminal | terminal-2", the list shown.
    void StackOfThree() {
        BootDefault();
        if (QTest::currentTestFailed()) {
            return;
        }
        ClickInMenu("terminal", "menuSplitRight");
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal-2") != nullptr &&
                                     SlotByInstance("terminal-2")->isVisible(),
                                 TIMEOUT_MS);
        ClickInMenu("terminal-2", "menuIntoTabs");
        if (QTest::currentTestFailed()) {
            return;
        }
        ClickInMenu("list", "menuIntoTabs");
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Shell_->CurrentLayout().Root.Kind,
                                  ELayoutNodeKind::Tabs, TIMEOUT_MS);
        QCOMPARE(Shell_->CurrentLayout().Root.Children.size(), size_t{3});
        QCOMPARE(Shell_->CurrentLayout().Root.Active, 0);
    }

private slots:
    void init() {
        Prepare();
        QVERIFY(QDir().mkpath(Dir_->filePath("home")));
        Home_.emplace("HOME", Dir_->filePath("home").toUtf8());
        Socket_.emplace("ROOST_SOCKET", SocketPath().toUtf8());
        Config_.emplace("ROOST_CONFIG", Dir_->filePath("config").toUtf8());
    }

    void cleanup() {
        Teardown();
        Config_.reset();
        Socket_.reset();
        Home_.reset();
    }

    void ClickingTheListTabGivesTheListTheKeyboard() {
        TerminalIntoTabs();
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(Cursor(), 0);
        ClickTab(0);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("list")->isVisible() &&
                                     SlotByInstance("list")->hasActiveFocus(),
                                 TIMEOUT_MS);
        QVERIFY(!SlotByInstance("terminal")->hasActiveFocus());
        Press(Qt::Key_Down);
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), 1, TIMEOUT_MS);
    }

    void CloseActsOnTheListAfterClickingItsTab() {
        TerminalIntoTabs();
        if (QTest::currentTestFailed()) {
            return;
        }
        ClickTab(0);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("list")->hasActiveFocus(),
                                 TIMEOUT_MS);
        QVERIFY(Api_->Commands().HasLocal("layout.close"));
        Api_->Commands().Run("layout.close");
        // What is left is the terminal, alone.
        QTRY_COMPARE_WITH_TIMEOUT(Shell_->CurrentLayout().Root.Kind,
                                  ELayoutNodeKind::Leaf, TIMEOUT_MS);
        QCOMPARE(Shell_->CurrentLayout().Root.Instance, QString("terminal"));
    }

    void TheChosenTabIsStoredAndShownAfterARestart() {
        TerminalIntoTabs();
        if (QTest::currentTestFailed()) {
            return;
        }
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        ClickTab(0);
        QTRY_COMPARE_WITH_TIMEOUT(StoredActive(), 0, TIMEOUT_MS);
        // The store tells of the write after roostd keeps it.
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        QCOMPARE(StoredLayout(), LayoutToJson(Shell_->CurrentLayout()));

        // The tab that is shown already is not written again: the next write
        // is that of the other tab, and a late write of this click would be a
        // third one.
        ClickTab(0);
        ClickTab(1);
        QTRY_COMPARE_WITH_TIMEOUT(StoredActive(), 1, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 2, TIMEOUT_MS);
        ClickTab(0);
        QTRY_COMPARE_WITH_TIMEOUT(StoredActive(), 0, TIMEOUT_MS);

        CloseWindow();
        StopDaemon();
        QVERIFY(StartDaemon());
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("list") != nullptr &&
                                     SlotByInstance("terminal") != nullptr &&
                                     SlotByInstance("list")->isVisible() &&
                                     !SlotByInstance("terminal")->isVisible(),
                                 TIMEOUT_MS);
        QCOMPARE(Shell_->CurrentLayout().Root.Active, 0);
    }

    void TheTabBarNeverHoldsTheKeyboard() {
        TerminalIntoTabs();
        if (QTest::currentTestFailed()) {
            return;
        }
        ClickTab(0);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("list")->hasActiveFocus(),
                                 TIMEOUT_MS);
        QList<QQuickItem*> bars;
        CollectByClass(Window()->contentItem(), "TabBar", bars);
        QCOMPARE(bars.size(), 1);
        // Neither the bar nor a button can take the keyboard, by a click (on
        // Linux; macOS gives a button the keyboard only by Tab) or by Tab.
        QCOMPARE(bars.first()->property("focusPolicy").toInt(),
                 int(Qt::NoFocus));
        for (QQuickItem* tab : TabButtons()) {
            QCOMPARE(tab->property("focusPolicy").toInt(), int(Qt::NoFocus));
        }
        for (int i = 0; i < 4; ++i) {
            Press(Qt::Key_Tab);
            QVERIFY(!bars.first()->hasActiveFocus());
            for (QQuickItem* tab : TabButtons()) {
                QVERIFY(!tab->hasActiveFocus());
            }
        }
        // The arrows do not switch the tab, shown or stored.
        Press(Qt::Key_Right);
        Press(Qt::Key_Left);
        Press(Qt::Key_Right);
        QTest::qWait(100);
        QVERIFY(SlotByInstance("list")->isVisible());
        QVERIFY(!SlotByInstance("terminal")->isVisible());
        QCOMPARE(Shell_->CurrentLayout().Root.Active, 0);
        QCOMPARE(StoredActive(), 0);
    }

    void ClickingTheThirdTabOfThreeFocusesItAndStoresIt() {
        StackOfThree();
        if (QTest::currentTestFailed()) {
            return;
        }
        ClickTab(2);
        QTRY_VERIFY_WITH_TIMEOUT(
            SlotByInstance("terminal-2")->isVisible() &&
                SlotByInstance("terminal-2")->hasActiveFocus(),
            TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(StoredActive(), 2, TIMEOUT_MS);
        QCOMPARE(StoredLayout(), LayoutToJson(Shell_->CurrentLayout()));
    }
};

QTEST_MAIN(LayoutTabsChoiceTest)
#include "layout_tabs_choice_test.moc"
