// The layout of the window is kept and comes back: two pairs of a list and a
// terminal, the divider of one dragged with the mouse, then roostd and the
// window engine stopped and started again over the same ROOST_CONFIG. The
// tree, the sizes, the links and the session each list had open are in place.
// A second window is not part of this: one window writes the layout.

#include <QDir>
#include <QQmlExtensionPlugin>
#include <QTest>

#include <optional>

#include "helpers/layout_window.hpp"
#include "helpers/scoped_env.hpp"

using namespace roost;
using namespace roost::testing;

Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

class LayoutRestoreTest : public LayoutWindow {
    Q_OBJECT

private:
    // A drag of the divider to the right of the slot of the list, with the
    // mouse.
    void DragDivider(const QString& list, int dx) {
        QQuickItem* slot = SlotByInstance(list);
        QVERIFY(slot != nullptr);
        const QPoint from =
            slot->mapToScene(QPointF(slot->width(), slot->height() / 2))
                .toPoint();
        QTest::mousePress(Window(), Qt::LeftButton, Qt::NoModifier, from);
        for (int step = 1; step <= 8; ++step) {
            QTest::mouseMove(Window(), from + QPoint(dx * step / 8, 0));
        }
        QTest::mouseRelease(Window(), Qt::LeftButton, Qt::NoModifier,
                            from + QPoint(dx, 0));
    }

    std::optional<ScopedEnv> Home_;
    std::optional<ScopedEnv> Socket_;
    std::optional<ScopedEnv> Config_;

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

    void TheDefaultLayoutIsWrittenOnlyWhenTheUserChangesIt() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        // Nothing the user did: nothing stored.
        QTest::qWait(300);
        QVERIFY(StoredLayout().isEmpty());
        QCOMPARE(Shell_->CurrentLayout(), DefaultLayout());
    }

    void TheLayoutTheSizesAndTheOpenSessionsComeBack() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Shell_->SetLayout(
            Stacked({Pair("list", "terminal"), Pair("list2", "terminal2")}));
        WaitForLayout({{"list", "terminal"}, {"list2", "terminal2"}});
        const QString first = IdIn("list", 1);
        const QString second = IdIn("list2", 2);
        QVERIFY(first != second);
        OpenIdIn("list", "terminal", first);
        OpenIdIn("list2", "terminal2", second);

        // The divider of the first pair: wider list.
        QCOMPARE(SlotByInstance("list")->width(), 320.0);
        DragDivider("list", 80);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("list")->width() > 380.0,
                                 TIMEOUT_MS);
        const int dragged = static_cast<int>(SlotByInstance("list")->width());
        QCOMPARE(SlotByInstance("list2")->width(), 320.0);

        // The patch is in the daemon, and both lists have written their views.
        const Layout before = Shell_->CurrentLayout();
        QCOMPARE(before.Root.Children[0].Sizes[0], LayoutSize::Px(dragged));
        QCOMPARE(before.Root.Children[1].Sizes[0], LayoutSize::Px(320));
        QTRY_COMPARE_WITH_TIMEOUT(StoredLayout(), LayoutToJson(before),
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(OpenedStored("list").contains(first),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(OpenedStored("list2").contains(second),
                                 TIMEOUT_MS);

        // Everything stops, and starts again over the same config.
        CloseWindow();
        StopDaemon();
        QVERIFY(StartDaemon());
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}, {"list2", "terminal2"}});
        QTRY_COMPARE_WITH_TIMEOUT(Shell_->CurrentLayout(), before, TIMEOUT_MS);
        QCOMPARE(SlotCount(), 4);
        QTRY_COMPARE_WITH_TIMEOUT(
            static_cast<int>(SlotByInstance("list")->width()), dragged,
            TIMEOUT_MS);
        QCOMPARE(SlotByInstance("list2")->width(), 320.0);
        // Each list has its own session open again, in its own terminal.
        QTRY_COMPARE_WITH_TIMEOUT(SelectedOf("list"), first, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(SelectedOf("list2"), second, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(TerminalOf("terminal")->status(),
                                  QString("open"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(TerminalOf("terminal2")->status(),
                                  QString("open"), TIMEOUT_MS);
        QCOMPARE(
            TerminalOf("terminal")->property("session").toMap().value("id"),
            QVariant(first));
        QCOMPARE(
            TerminalOf("terminal2")->property("session").toMap().value("id"),
            QVariant(second));
        QVERIFY(Problems().isEmpty());
    }

    void ALayoutWithATabStackIsShownAndItsTabsAreInPlace() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        // A stack of tabs, which the window has no way to make yet, as a
        // saved layout could hold it: the second tab is the shown one.
        LayoutNode tabs;
        tabs.Kind = ELayoutNodeKind::Tabs;
        tabs.Active = 1;
        tabs.Children = {Pair("list", "terminal"), Pair("list2", "terminal2")};
        Layout layout;
        layout.Root = std::move(tabs);
        Shell_->SetLayout(layout);
        WaitForLayout({{"list", "terminal"}, {"list2", "terminal2"}}, false);
        QCOMPARE(SlotCount(), 4);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("list2")->isVisible(),
                                 TIMEOUT_MS);
        QVERIFY(!SlotByInstance("list")->isVisible());
        QCOMPARE(Shell_->CurrentLayout(), layout);
    }
};

QTEST_MAIN(LayoutRestoreTest)
#include "layout_restore_test.moc"
