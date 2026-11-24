// The panels are cards, and the card of the panel that had the keyboard last
// has the focus ring (PanelCard.qml): the ring follows the keyboard from one
// panel to the other and stays where it was when a popup takes the keyboard.
// In the real window against a real roostd.
//
// With ROOST_TEST_SNAPSHOTS set to a directory, a split and a stack of tabs
// save a picture of the window there, for a person to look at.

#include <QDir>
#include <QPainter>
#include <QQuickItemGrabResult>
#include <QSignalSpy>
#include <QTest>
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
        QVERIFY(ready.wait(TIMEOUT_MS) || !ready.isEmpty());
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
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        // A session in the terminal, so that the picture shows it in the card.
        OpenIn("list", "terminal", 0);
        if (QTest::currentTestFailed()) {
            return;
        }

        ListViewOf("list")->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(Ringed("list") && !Ringed("terminal"),
                                 TIMEOUT_MS);
        Snapshot("split-list-focused");
        if (QTest::currentTestFailed()) {
            return;
        }

        TerminalOf("terminal")->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(Ringed("terminal") && !Ringed("list"),
                                 TIMEOUT_MS);
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
        QVERIFY(Ringed("terminal"));
        QVERIFY(!Ringed("list"));
    }

    void TheStackOfTabsIsOneCardWithTheRing() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        LayoutNode list;
        list.Instance = "list";
        list.Panel = LIST;
        LayoutNode terminal;
        terminal.Instance = "terminal";
        terminal.Panel = TERMINAL;
        terminal.Links.insert(
            "session", {.From = "list", .Output = "selected", .Problem = {}});
        Layout layout;
        layout.Root.Kind = ELayoutNodeKind::Tabs;
        layout.Root.Active = 0;
        layout.Root.Children = {std::move(list), std::move(terminal)};
        Shell_->SetLayout(layout);
        WaitForSlots({"list"});

        ListViewOf("list")->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(Ringed("list"), TIMEOUT_MS);
        // The card is the stack's: the page has none of its own.
        QQuickItem* card = CardOf("list");
        QVERIFY(card != nullptr);
        QCOMPARE(card->objectName(), QString("tabsCard"));
        Snapshot("tabs");
    }
};

QTEST_MAIN(LayoutCardFocusTest)
#include "layout_card_focus_test.moc"
