// Children of a split that are all "fill" share the room equally, as a user
// sees it: the second terminal made by "Split right" or "Split down" is as big
// as the first, not a 40 px strip. A drag of the divider between two of them
// is kept in the layout document and comes back after a restart, the divider
// is grabbed a few pixels beside its line, and the shares follow the size of
// the window. The real window against a real roostd, the menu by mouse.

#include <QDir>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTest>
#include <optional>

#include "helpers/layout_window.hpp"
#include "helpers/scoped_env.hpp"

using namespace roost;
using namespace roost::testing;

class LayoutFillSharesTest : public LayoutWindow {
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

    void BootDefault() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
    }

    // The slot is shown, and has the room of a terminal.
    void WaitForSlot(const QString& instance) {
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance(instance) != nullptr &&
                                     SlotByInstance(instance)->isVisible() &&
                                     SlotByInstance(instance)->width() > 1,
                                 TIMEOUT_MS);
    }

    double Wide(const QString& instance) {
        return SlotByInstance(instance)->width();
    }
    double High(const QString& instance) {
        return SlotByInstance(instance)->height();
    }
    double Left(const QString& instance) {
        return SlotByInstance(instance)->mapToScene(QPointF(0, 0)).x();
    }

    // The two slots are as wide (or as high) as each other, give or take 2 px,
    // and neither is a strip.
    void VerifyEqual(const QString& a, const QString& b, bool wide) {
        const auto size = [&](const QString& instance) {
            return wide ? Wide(instance) : High(instance);
        };
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(size(a) - size(b)) <= 2 && size(a) > 100,
                                 TIMEOUT_MS);
    }

    // A drag with the mouse from `from` by `dx`, in steps.
    void Drag(QPoint from, int dx) {
        QTest::mousePress(Window(), Qt::LeftButton, Qt::NoModifier, from);
        for (int step = 1; step <= 8; ++step) {
            QTest::mouseMove(Window(), from + QPoint(dx * step / 8, 0));
        }
        QTest::mouseRelease(Window(), Qt::LeftButton, Qt::NoModifier,
                            from + QPoint(dx, 0));
    }

    // The point on the divider to the right of the slot, `beside` pixels to
    // the right of the line (negative: to the left).
    QPoint DividerRightOf(const QString& instance, int beside) {
        QQuickItem* slot = SlotByInstance(instance);
        return slot->mapToScene(QPointF(slot->width() + beside, 60)).toPoint();
    }

    QJsonArray RootSizes() {
        return StoredLayout().value("root").toObject().value("sizes").toArray();
    }

    // Split right on the terminal from the default layout; both terminals up.
    void SplitTerminalRight() {
        BootDefault();
        if (QTest::currentTestFailed()) {
            return;
        }
        ClickInMenu("terminal", "menuSplitRight");
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForSlot("terminal-2");
        VerifyEqual("terminal", "terminal-2", true);
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

    void SplitRightGivesTheTwoTerminalsHalfEach() {
        SplitTerminalRight();
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(Wide("list"), 320.0);
        // Side by side, the list, the old terminal, the new one.
        QVERIFY(Left("list") < Left("terminal"));
        QVERIFY(Left("terminal") < Left("terminal-2"));
    }

    void SplitDownGivesTheTwoTerminalsHalfEach() {
        BootDefault();
        if (QTest::currentTestFailed()) {
            return;
        }
        ClickInMenu("terminal", "menuSplitDown");
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForSlot("terminal-2");
        VerifyEqual("terminal", "terminal-2", false);
        QCOMPARE(Wide("terminal"), Wide("terminal-2"));
    }

    void TheListAndTheTerminalOutOfTabsShareTheWidth() {
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
        ClickInMenu("terminal", "menuOutOfTabs");
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForSlot("list");
        WaitForSlot("terminal");
        // Out of tabs the panel has the rest, so has the list when it is the
        // one that has no size: the two of them are "fill" and are equal.
        const Layout layout = Shell_->CurrentLayout();
        QCOMPARE(layout.Root.Kind, ELayoutNodeKind::Split);
        QCOMPARE(layout.Root.Children.size(), size_t{2});
        if (layout.Root.Sizes[0] == LayoutSize::Rest() &&
            layout.Root.Sizes[1] == LayoutSize::Rest()) {
            VerifyEqual("list", "terminal", true);
        } else {
            QVERIFY(Wide("terminal") > 100);
            QVERIFY(Wide("list") > 100);
        }
    }

    void ADragBetweenTwoFillPanelsIsKeptAndComesBackAfterARestart() {
        SplitTerminalRight();
        if (QTest::currentTestFailed()) {
            return;
        }
        const double before = Wide("terminal");
        Drag(DividerRightOf("terminal", 0), 100);
        QTRY_VERIFY_WITH_TIMEOUT(Wide("terminal") > before + 80, TIMEOUT_MS);
        const int dragged = static_cast<int>(Wide("terminal"));
        QVERIFY(Wide("terminal-2") >= 40);

        // The document says the pixels of the dragged panel, and the filling
        // one is the last.
        const Layout kept = Shell_->CurrentLayout();
        QCOMPARE(kept.Root.Sizes.size(), size_t{3});
        QCOMPARE(kept.Root.Sizes[1], LayoutSize::Px(dragged));
        QCOMPARE(kept.Root.Sizes[2], LayoutSize::Rest());
        QTRY_COMPARE_WITH_TIMEOUT(StoredLayout(), LayoutToJson(kept),
                                  TIMEOUT_MS);
        QCOMPARE(RootSizes().at(1).toInt(), dragged);
        QCOMPARE(RootSizes().at(2).toString(), QString("fill"));

        CloseWindow();
        StopDaemon();
        QVERIFY(StartDaemon());
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForSlot("terminal");
        WaitForSlot("terminal-2");
        QTRY_COMPARE_WITH_TIMEOUT(static_cast<int>(Wide("terminal")), dragged,
                                  TIMEOUT_MS);
        QVERIFY(Wide("terminal-2") > 100);
    }

    void APressBesideTheLineStillTakesTheDivider() {
        SplitTerminalRight();
        if (QTest::currentTestFailed()) {
            return;
        }
        // The line is 1 px; 2 px to either side of it is the divider too.
        for (const int beside : {-2, 2}) {
            const double before = Wide("terminal");
            Drag(DividerRightOf("terminal", beside), 60);
            QTRY_VERIFY_WITH_TIMEOUT(Wide("terminal") > before + 40,
                                     TIMEOUT_MS);
            // Back to where it was, for the next press.
            Drag(DividerRightOf("terminal", 0),
                 -static_cast<int>(Wide("terminal") - before));
            QTRY_VERIFY_WITH_TIMEOUT(qAbs(Wide("terminal") - before) <= 2,
                                     TIMEOUT_MS);
        }
        // Far from the line (8 px) it is the terminal's, not the divider's.
        const double before = Wide("terminal");
        Drag(DividerRightOf("terminal", -8), 60);
        QTest::qWait(200);
        QCOMPARE(Wide("terminal"), before);
    }

    void ResizingTheWindowKeepsTheSharesEqual() {
        SplitTerminalRight();
        if (QTest::currentTestFailed()) {
            return;
        }
        const int width = Window()->width();
        Window()->resize(width + 240, Window()->height());
        QTRY_VERIFY_WITH_TIMEOUT(Wide("terminal") > 300, TIMEOUT_MS);
        VerifyEqual("terminal", "terminal-2", true);
        QCOMPARE(Wide("list"), 320.0);
        Window()->resize(width - 160, Window()->height());
        QTRY_VERIFY_WITH_TIMEOUT(Wide("terminal") < 300, TIMEOUT_MS);
        VerifyEqual("terminal", "terminal-2", true);
    }
};

QTEST_MAIN(LayoutFillSharesTest)
#include "layout_fill_shares_test.moc"
