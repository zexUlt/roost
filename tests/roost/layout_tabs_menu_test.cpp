// Into tabs and out of tabs from the panel menu, as a user does it: a right
// click on the panel, a left click on the item, in the real window against a
// real roostd. Out of tabs gives back the window the user had: the list on the
// left of the terminal, whichever of the two went into tabs.
//
// With ROOST_TEST_SNAPSHOTS set to a directory, each step saves a picture of
// the window there, for a person to look at.

#include <QSignalSpy>
#include <QTest>
#include <optional>

#include "helpers/layout_window.hpp"
#include "helpers/scoped_env.hpp"
#include "helpers/snapshot.hpp"

using namespace roost;
using namespace roost::testing;

namespace {

QStringList Instances(const LayoutNode& node) {
    if (node.Kind == ELayoutNodeKind::Leaf) {
        return {node.Instance};
    }
    QStringList all;
    for (const LayoutNode& child : node.Children) {
        all.append(Instances(child));
    }
    return all;
}

}  // namespace

class LayoutTabsMenuTest : public LayoutWindow {
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

    // Saves a picture of the window as <case>-<step>.png when asked to.
    void Snapshot(const QString& step) {
        QVERIFY(SaveSnapshot(Window(), step));
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
        QVERIFY(SaveSnapshot(Window(), QString("menu-") + item));
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        QTest::mouseClick(
            Window(), Qt::LeftButton, Qt::NoModifier,
            entry->mapToScene(QPointF(entry->width() / 2, entry->height() / 2))
                .toPoint());
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!popup->property("visible").toBool(),
                                 TIMEOUT_MS);
    }

    // What the user sees: the list and the terminal side by side, the list
    // on the left, as in the layout the window started with. `stored`: roostd
    // keeps that layout (none is stored before the first edit).
    void VerifyListLeftOfTerminal(bool stored) {
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("list") != nullptr &&
                                     SlotByInstance("terminal") != nullptr &&
                                     SlotByInstance("list")->isVisible() &&
                                     SlotByInstance("terminal")->isVisible(),
                                 TIMEOUT_MS);
        const auto left = [this](const QString& instance) {
            QQuickItem* slot = SlotByInstance(instance);
            return slot->mapToScene(QPointF(0, 0)).x();
        };
        QTRY_VERIFY_WITH_TIMEOUT(left("list") < left("terminal"), TIMEOUT_MS);
        const Layout layout = Shell_->CurrentLayout();
        QCOMPARE(layout.Root.Kind, ELayoutNodeKind::Split);
        QVERIFY(layout.Root.Horizontal);
        QCOMPARE(Instances(layout.Root), (QStringList{"list", "terminal"}));
        if (stored) {
            QCOMPARE(StoredLayout(), LayoutToJson(layout));
        }
    }

    void IntoTabsAndOut(const QString& instance) {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        VerifyListLeftOfTerminal(false);
        Snapshot("1-start");
        if (QTest::currentTestFailed()) {
            return;
        }

        ClickInMenu(instance, "menuIntoTabs");
        if (QTest::currentTestFailed()) {
            return;
        }
        // One stack of the two, the moved panel shown.
        QTRY_COMPARE_WITH_TIMEOUT(Shell_->CurrentLayout().Root.Kind,
                                  ELayoutNodeKind::Tabs, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance(instance) != nullptr &&
                                     SlotByInstance(instance)->isVisible(),
                                 TIMEOUT_MS);
        Snapshot("2-tabs");

        ClickInMenu(instance, "menuOutOfTabs");
        if (QTest::currentTestFailed()) {
            return;
        }
        Snapshot("3-out");
        VerifyListLeftOfTerminal(true);
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

    // The case of the report: the list goes into tabs and out again.
    void TheListIntoTabsAndOutStaysOnTheLeft() { IntoTabsAndOut("list"); }

    void TheTerminalIntoTabsAndOutStaysOnTheRight() {
        IntoTabsAndOut("terminal");
    }
};

QTEST_MAIN(LayoutTabsMenuTest)
#include "layout_tabs_menu_test.moc"
