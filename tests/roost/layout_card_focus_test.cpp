// The panels are cards, and the card of the panel that had the keyboard last
// has the focus ring (PanelCard.qml): the ring follows the keyboard from one
// panel to the other and stays where it was when a popup takes the keyboard.
// In the real window against a real roostd.
//
// Inside a stack of tabs there is one card, the stack's, with one ring,
// whatever lies on its pages. With ROOST_TEST_SNAPSHOTS set to a directory, a
// split, a stack of tabs and a split on a page of a stack save a picture of
// the window there, for a person to look at.

#include <QDir>
#include <QPainter>
#include <QQuickItemGrabResult>
#include <QSignalSpy>
#include <QTest>
#include <algorithm>
#include <functional>
#include <optional>

#include "helpers/layout_window.hpp"
#include "helpers/scoped_env.hpp"

using namespace roost;
using namespace roost::testing;

class LayoutCardFocusTest : public LayoutWindow {
    Q_OBJECT

private:
    std::optional<ScopedEnv> Home_;
    std::optional<ScopedEnv> Socket_;
    std::optional<ScopedEnv> Config_;

    // Saves a picture of the window as <case>.png when asked to; the content
    // item, not the window: grabWindow distorts the terminal offscreen.
    void Snapshot(const QString& name) {
        const QString directory = qEnvironmentVariable("ROOST_TEST_SNAPSHOTS");
        if (directory.isEmpty()) {
            return;
        }
        QVERIFY(QDir().mkpath(directory));
        QSharedPointer<QQuickItemGrabResult> result =
            Window()->contentItem()->grabToImage();
        QVERIFY(!result.isNull());
        QSignalSpy ready(result.data(), &QQuickItemGrabResult::ready);
        // The result may be ready already: wait only if it is not.
        if (ready.isEmpty()) {
            QVERIFY(ready.wait(TIMEOUT_MS));
        }
        // The content item has no colour of its own: the picture is put on
        // the colour of the window, as the user sees it.
        QImage picture(result->image().size(), QImage::Format_ARGB32);
        picture.fill(Window()->color());
        QPainter painter(&picture);
        painter.drawImage(0, 0, result->image());
        painter.end();
        QVERIFY(picture.save(
            QDir(directory).filePath(name + QStringLiteral(".png"))));
    }

    // The card that holds the panel: the nearest one above its slot that
    // draws (a page of a stack sits in the card of the stack).
    QQuickItem* CardOf(const QString& instance) {
        QQuickItem* slot = SlotByInstance(instance);
        for (QQuickItem* item = slot; item != nullptr;
             item = item->parentItem()) {
            if (item->objectName() != QLatin1String("panelCard") &&
                item->objectName() != QLatin1String("tabsCard")) {
                continue;
            }
            if (!item->property("bare").toBool()) {
                return item;
            }
        }
        return nullptr;
    }

    // Whether the ring is drawn around the card of the panel.
    bool Ringed(const QString& instance) {
        QQuickItem* card = CardOf(instance);
        QQuickItem* ring = card == nullptr
                               ? nullptr
                               : card->findChild<QQuickItem*>("panelCardRing");
        return ring != nullptr && ring->isVisible();
    }

    // How many items of the window are drawn with the name: the cards that
    // draw (a bare one hides its face), their rings.
    int Shown(const char* name) {
        int count = 0;
        for (QQuickItem* item : Root()->findChildren<QQuickItem*>(name)) {
            count += item->isVisible() ? 1 : 0;
        }
        return count;
    }

    // The ring is on the card of `instance` and on no other card, and the
    // card is the only one of its stack.
    bool RingOnlyOn(const QString& instance) {
        return Ringed(instance) && Shown("panelCardRing") == 1;
    }

    // The keyboard goes to the panel, as the user's click does.
    void Focus(const QString& instance) {
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance(instance) != nullptr &&
                                     SlotByInstance(instance)->isVisible(),
                                 TIMEOUT_MS);
        QQuickItem* slot = SlotByInstance(instance);
        slot->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(slot->hasActiveFocus(), TIMEOUT_MS);
    }

    // A panel of the type, linked to the list, as in the default layout.
    static LayoutNode Terminal(const QString& instance) {
        LayoutNode node;
        node.Instance = instance;
        node.Panel = TERMINAL;
        node.Links.insert(
            "session", {.From = "list", .Output = "selected", .Problem = {}});
        return node;
    }

    static LayoutNode List() {
        LayoutNode node;
        node.Instance = "list";
        node.Panel = LIST;
        return node;
    }

    static LayoutNode Tabs(std::vector<LayoutNode> pages, int active = 0) {
        LayoutNode node;
        node.Kind = ELayoutNodeKind::Tabs;
        node.Active = active;
        node.Children = std::move(pages);
        return node;
    }

    static LayoutNode Split(std::vector<LayoutNode> parts) {
        LayoutNode node;
        node.Kind = ELayoutNodeKind::Split;
        node.Horizontal = true;
        for (size_t i = 0; i < parts.size(); ++i) {
            node.Sizes.push_back(LayoutSize::Rest());
        }
        node.Children = std::move(parts);
        return node;
    }

    // The window shows the layout, and the panels named are placed.
    void Show(LayoutNode root, const QStringList& shown) {
        Layout layout;
        layout.Root = std::move(root);
        Shell_->SetLayout(layout);
        WaitForSlots(shown);
    }

    // The window is up with the default layout, a list and a terminal.
    void BootPair() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
    }

    // The button of the tab of the window with the number, left to right.
    QQuickItem* TabButton(int index) {
        QList<QQuickItem*> buttons;
        const std::function<void(QQuickItem*)> collect = [&](QQuickItem* item) {
            for (QQuickItem* child : item->childItems()) {
                const QString cls = child->metaObject()->className();
                if (cls.startsWith("TabButton") && child->isVisible()) {
                    buttons.append(child);
                }
                collect(child);
            }
        };
        collect(Window()->contentItem());
        std::sort(buttons.begin(), buttons.end(),
                  [](QQuickItem* a, QQuickItem* b) {
                      return a->mapToScene(QPointF(0, 0)).x() <
                             b->mapToScene(QPointF(0, 0)).x();
                  });
        return buttons.size() > index ? buttons.at(index) : nullptr;
    }

    // The handles of the splits, as the split view finds them.
    QList<QQuickItem*> Handles() {
        QList<QQuickItem*> handles;
        for (QQuickItem* item :
             Root()->findChildren<QQuickItem*>("splitHandle")) {
            if (item->isVisible()) {
                handles.append(item);
            }
        }
        return handles;
    }

    // Whether a press `along` the split (from the start of the handle) in the
    // middle of it takes the handle.
    static bool Takes(QQuickItem* handle, qreal along) {
        return handle->contains(QPointF(along, handle->height() / 2));
    }

    void CloseRing(const QString& instance) {
        Focus(instance);
        QVERIFY(Api_->Commands().HasLocal("layout.close"));
        Api_->Commands().Run("layout.close");
    }

    void WaitForSlots(const QStringList& instances) {
        for (const QString& instance : instances) {
            QTRY_VERIFY_WITH_TIMEOUT(
                SlotByInstance(instance) != nullptr &&
                    SlotByInstance(instance)->isVisible() &&
                    SlotByInstance(instance)->width() > 0,
                TIMEOUT_MS);
        }
    }

    QObject* Palette() { return Root()->findChild<QObject*>("commandPalette"); }

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

    void TheRingFollowsTheKeyboardAndStaysWhenAPopupTakesIt() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        // A session in the terminal, so that the picture shows it in the card.
        // Enter on a row gives the keyboard to the terminal it feeds.
        OpenIn("list", "terminal", 0);
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(RingOnlyOn("terminal"), TIMEOUT_MS);

        // Every step below moves the ring to the card that does not have it.
        Focus("list");
        QTRY_VERIFY_WITH_TIMEOUT(RingOnlyOn("list"), TIMEOUT_MS);
        Snapshot("split-list-focused");
        if (QTest::currentTestFailed()) {
            return;
        }

        Focus("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(RingOnlyOn("terminal"), TIMEOUT_MS);
        Snapshot("split-terminal-focused");
        if (QTest::currentTestFailed()) {
            return;
        }

        // The palette takes the keyboard: the panel does not have it, and
        // the ring stays on the card that had it last.
        QVERIFY(QMetaObject::invokeMethod(Root(), "openPalette"));
        QTRY_VERIFY_WITH_TIMEOUT(Palette()->property("visible").toBool(),
                                 TIMEOUT_MS);
        QQuickItem* field = Palette()->findChild<QQuickItem*>("paletteField");
        QVERIFY(field != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(field->hasActiveFocus(), TIMEOUT_MS);
        QVERIFY(!TerminalOf("terminal")->hasActiveFocus());
        QVERIFY(RingOnlyOn("terminal"));
    }

    // The stack is one card: the ring moves between it and a panel beside
    // it, whichever of its pages has the keyboard.
    void TheStackOfTabsIsOneCardWithTheRing() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        Show(Split({List(),
                    Tabs({Terminal("terminal"), Terminal("terminal-2")})}),
             {"list", "terminal"});
        if (QTest::currentTestFailed()) {
            return;
        }

        Focus("list");
        QTRY_VERIFY_WITH_TIMEOUT(RingOnlyOn("list"), TIMEOUT_MS);
        Focus("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(RingOnlyOn("terminal"), TIMEOUT_MS);
        // The card is the stack's: the page has none of its own.
        QQuickItem* card = CardOf("terminal");
        QVERIFY(card != nullptr);
        QCOMPARE(card->objectName(), QString("tabsCard"));
        QCOMPARE(Shown("panelCardFace"), 2);
        Snapshot("tabs");
        if (QTest::currentTestFailed()) {
            return;
        }

        // The ring goes back to the list, and to the stack again.
        Focus("list");
        QTRY_VERIFY_WITH_TIMEOUT(RingOnlyOn("list"), TIMEOUT_MS);
        Focus("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(RingOnlyOn("terminal"), TIMEOUT_MS);
    }

    // The tab is switched with the ring on the stack: the panel on the new
    // page has the keyboard, and the ring stays on the one card of the stack.
    void SwitchingTheTabKeepsTheRingOnTheStack() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        Show(Split({List(),
                    Tabs({Terminal("terminal"), Terminal("terminal-2")})}),
             {"list", "terminal"});
        if (QTest::currentTestFailed()) {
            return;
        }
        Focus("list");
        QTRY_VERIFY_WITH_TIMEOUT(RingOnlyOn("list"), TIMEOUT_MS);

        // A click on the second tab: its panel takes the keyboard, and the
        // ring comes to the stack from the list.
        QQuickItem* button = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (button = TabButton(1)) != nullptr && button->width() > 0,
            TIMEOUT_MS);
        QTest::mouseClick(
            Window(), Qt::LeftButton, Qt::NoModifier,
            button
                ->mapToScene(QPointF(button->width() / 2, button->height() / 2))
                .toPoint());
        QTRY_VERIFY_WITH_TIMEOUT(
            SlotByInstance("terminal-2")->isVisible() &&
                SlotByInstance("terminal-2")->hasActiveFocus(),
            TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(RingOnlyOn("terminal-2"), TIMEOUT_MS);
        QCOMPARE(CardOf("terminal-2")->objectName(), QString("tabsCard"));

        // Back to the first tab: the ring stays, on the same card.
        QQuickItem* stack = CardOf("terminal-2");
        QTRY_VERIFY_WITH_TIMEOUT(
            (button = TabButton(0)) != nullptr && button->width() > 0,
            TIMEOUT_MS);
        QTest::mouseClick(
            Window(), Qt::LeftButton, Qt::NoModifier,
            button
                ->mapToScene(QPointF(button->width() / 2, button->height() / 2))
                .toPoint());
        QTRY_VERIFY_WITH_TIMEOUT(
            SlotByInstance("terminal")->isVisible() &&
                SlotByInstance("terminal")->hasActiveFocus(),
            TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(RingOnlyOn("terminal"), TIMEOUT_MS);
        QCOMPARE(CardOf("terminal"), stack);
    }

    // A page that is a split of two panels: one card with one ring, the
    // stack's, while either panel has the keyboard; the divider is a line.
    void ASplitOnAPageOfTheStackIsInTheCardOfTheStack() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        Show(Tabs({Split({List(), Terminal("terminal")}),
                   Terminal("terminal-2")}),
             {"list", "terminal"});
        if (QTest::currentTestFailed()) {
            return;
        }

        Focus("list");
        QTRY_VERIFY_WITH_TIMEOUT(RingOnlyOn("list"), TIMEOUT_MS);
        QCOMPARE(CardOf("list")->objectName(), QString("tabsCard"));
        QCOMPARE(Shown("panelCardFace"), 1);
        QCOMPARE(Shown("panelCardOutline"), 1);
        Snapshot("nested");
        if (QTest::currentTestFailed()) {
            return;
        }
        Focus("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(RingOnlyOn("terminal"), TIMEOUT_MS);
        QCOMPARE(CardOf("terminal"), CardOf("list"));
        QCOMPARE(Shown("panelCardFace"), 1);

        // The divider of the split in the stack is a line of 1 px that takes
        // a press up to 2 px to either side, as before the cards.
        const QList<QQuickItem*> handles = Handles();
        QCOMPARE(handles.size(), 1);
        QQuickItem* handle = handles.first();
        QCOMPARE(handle->width(), 1.0);
        QVERIFY(Takes(handle, -2));
        QVERIFY(Takes(handle, 0));
        QVERIFY(Takes(handle, 2));
        QVERIFY(!Takes(handle, -3));
        QVERIFY(!Takes(handle, 3));
    }

    // A stack on a page of a stack has no card either.
    void AStackOnAPageOfTheStackIsInTheCardOfTheStack() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        Show(Tabs({Tabs({List(), Terminal("terminal")}),
                   Terminal("terminal-2")}),
             {"list"});
        if (QTest::currentTestFailed()) {
            return;
        }
        Focus("list");
        QTRY_VERIFY_WITH_TIMEOUT(RingOnlyOn("list"), TIMEOUT_MS);
        QCOMPARE(Shown("panelCardFace"), 1);
        QCOMPARE(Shown("panelCardOutline"), 1);
        QCOMPARE(CardOf("list")->objectName(), QString("tabsCard"));
        // The card is the outer stack's: the inner one is bare.
        for (QQuickItem* card : Root()->findChildren<QQuickItem*>("tabsCard")) {
            QCOMPARE(card->property("bare").toBool(), card != CardOf("list"));
        }
    }

    // The gap between two cards is the handle, and the handle takes a press
    // in the gap and 2 px into each card (the catch of #224 is 2 px to either
    // side of the line, and the edge of the card is that line), not farther.
    void TheHandleOfASplitOfCardsTakesTheGapAndTwoPixelsOfTheCards() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        const QList<QQuickItem*> handles = Handles();
        QCOMPARE(handles.size(), 1);
        QQuickItem* handle = handles.first();
        QCOMPARE(handle->width(), 12.0);
        QVERIFY(Takes(handle, 0));
        QVERIFY(Takes(handle, 11));
        QVERIFY(Takes(handle, -2));
        QVERIFY(Takes(handle, 13));
        QVERIFY(!Takes(handle, -3));
        QVERIFY(!Takes(handle, 14));
    }

    // The ringed panel is closed: the shell hands the keyboard to the
    // neighbour, and the ring lands there.
    void ClosingThePanelWithTheRingMovesItToTheNeighbour() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        Show(Split({List(), Terminal("terminal"), Terminal("terminal-2")}),
             {"list", "terminal", "terminal-2"});
        if (QTest::currentTestFailed()) {
            return;
        }
        Focus("terminal-2");
        QTRY_VERIFY_WITH_TIMEOUT(RingOnlyOn("terminal-2"), TIMEOUT_MS);
        CloseRing("terminal-2");
        // The one before it in the split.
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal-2") == nullptr,
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(RingOnlyOn("terminal"), TIMEOUT_MS);
        QVERIFY(SlotByInstance("terminal")->hasActiveFocus());
    }

    // A layout that has no panel of the ring any more (a restore, a change
    // by a plugin) names no neighbour: the first panel takes the keyboard and
    // the ring, as at start, and no ring is left on a panel that is gone.
    void AReplacedLayoutWithoutThePanelOfTheRingGivesTheFirstPanelTheRing() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        Show(Split({List(), Terminal("terminal")}), {"list", "terminal"});
        if (QTest::currentTestFailed()) {
            return;
        }
        Focus("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(RingOnlyOn("terminal"), TIMEOUT_MS);
        Show(Split({List(), Terminal("terminal-3")}), {"list", "terminal-3"});
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(
            SlotByInstance("list")->hasActiveFocus() && RingOnlyOn("list"),
            TIMEOUT_MS);
    }

    // The ringed panel is moved from a split into tabs: the ring goes to the
    // card of the stack, and the panel has none of its own.
    void MovingThePanelWithTheRingIntoTabsMovesTheRingToTheStack() {
        BootPair();
        if (QTest::currentTestFailed()) {
            return;
        }
        Focus("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(RingOnlyOn("terminal"), TIMEOUT_MS);
        QCOMPARE(CardOf("terminal")->objectName(), QString("panelCard"));
        QQuickItem* before = CardOf("terminal");

        QVERIFY(Api_->Commands().HasLocal("layout.intoTabs"));
        Api_->Commands().Run("layout.intoTabs");
        QTRY_COMPARE_WITH_TIMEOUT(Shell_->CurrentLayout().Root.Kind,
                                  ELayoutNodeKind::Tabs, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal")->hasActiveFocus() &&
                                     RingOnlyOn("terminal"),
                                 TIMEOUT_MS);
        QCOMPARE(CardOf("terminal")->objectName(), QString("tabsCard"));
        QVERIFY(CardOf("terminal") != before);
        QCOMPARE(Shown("panelCardFace"), 1);
    }
};

QTEST_MAIN(LayoutCardFocusTest)
#include "layout_card_focus_test.moc"
