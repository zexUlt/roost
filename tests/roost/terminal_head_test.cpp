// The head of the terminal panel (plugin `terminal`): the title and the meta
// line of the shown session, and the buttons "Lists" and "Commands". Real
// roostd with the real Claude plugin behind a fake `claude`, the real window;
// the buttons are clicked with the mouse through the window.

#include <QDir>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QTest>

#include <optional>

#include "helpers/layout_window.hpp"
#include "helpers/scoped_env.hpp"
#include "helpers/snapshot.hpp"

using namespace roost;
using namespace roost::testing;

Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

class TerminalHeadTest : public LayoutWindow {
    Q_OBJECT

private:
    QQuickItem* HeadItem(const QString& terminal, const char* name) {
        QQuickItem* panel = PanelByInstance(terminal);
        return panel == nullptr ? nullptr : panel->findChild<QQuickItem*>(name);
    }

    QString TitleOf(const QString& terminal) {
        const QQuickItem* item = HeadItem(terminal, "terminalTitle");
        return item == nullptr ? QString() : item->property("text").toString();
    }

    QString MetaOf(const QString& terminal) {
        const QQuickItem* item = HeadItem(terminal, "terminalMeta");
        return item == nullptr ? QString() : item->property("text").toString();
    }

    // A click of the mouse in the middle of the button, once it is laid out.
    void Click(const QString& terminal, const char* name) {
        QTRY_VERIFY_WITH_TIMEOUT(HeadItem(terminal, name) != nullptr &&
                                     HeadItem(terminal, name)->isVisible() &&
                                     HeadItem(terminal, name)->width() > 1,
                                 TIMEOUT_MS);
        const QQuickItem* button = HeadItem(terminal, name);
        const QPoint at =
            button
                ->mapToScene(QPointF(button->width() / 2, button->height() / 2))
                .toPoint();
        QTest::mouseClick(Window(), Qt::LeftButton, Qt::NoModifier, at);
    }

    bool Shown(const QString& instance) {
        const QQuickItem* slot = SlotByInstance(instance);
        return slot != nullptr && slot->isVisible() && slot->width() > 1;
    }

    // Two pairs, so that "Lists" has two lists to hide.
    void BootTwoPairs() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Shell_->SetLayout(
            Stacked({Pair("list", "terminal"), Pair("list2", "terminal2")}));
        WaitForLayout({{"list", "terminal"}, {"list2", "terminal2"}});
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

    void TheTitleAndTheMetaFollowTheSelectedSession() {
        BootTwoPairs();
        if (QTest::currentTestFailed()) {
            return;
        }
        // Without a session: the placeholder and no meta.
        QCOMPARE(TitleOf("terminal"), QString("No session"));
        QCOMPARE(MetaOf("terminal"), QString());
        QVERIFY(SaveSnapshot(Window(), "without-session"));

        OpenIdIn("list", "terminal", ALPHA);
        QTRY_COMPARE_WITH_TIMEOUT(TitleOf("terminal"),
                                  QString("Fix the flaky test"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(MetaOf("terminal"),
                                  QString("working · /work/alpha"), TIMEOUT_MS);
        // The other terminal shows nothing yet.
        QCOMPARE(TitleOf("terminal2"), QString("No session"));
        QVERIFY(SaveSnapshot(Window(), "with-session"));

        // A session that waits is called so, as in the list.
        OpenIdIn("list", "terminal", BETA);
        QTRY_COMPARE_WITH_TIMEOUT(TitleOf("terminal"), QString("Review the PR"),
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(MetaOf("terminal"),
                                  QString("waiting · /work/beta"), TIMEOUT_MS);
        QCOMPARE(TitleOf("terminal2"), QString("No session"));
    }

    void ListsHidesAndShowsEveryListThroughTheCommand() {
        BootTwoPairs();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenIdIn("list", "terminal", ALPHA);
        QVERIFY(!Root()->property("listsHidden").toBool());
        QVERIFY(Shown("list") && Shown("list2"));
        const qreal width = SlotByInstance("terminal")->width();

        Click("terminal", "listsButton");
        QTRY_VERIFY_WITH_TIMEOUT(Root()->property("listsHidden").toBool(),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!SlotByInstance("list")->isVisible() &&
                                     !SlotByInstance("list2")->isVisible(),
                                 TIMEOUT_MS);
        QVERIFY(Shell_->CurrentLayout().ListsHidden);
        // The terminal has taken the room of the lists: the buttons are where
        // they stay, and the next click lands on one.
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal")->width() > width,
                                 TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(StoredLayout().value("listsHidden"),
                                  QJsonValue(true), TIMEOUT_MS);
        QVERIFY(SaveSnapshot(Window(), "lists-hidden"));

        // The same button brings them back.
        Click("terminal", "listsButton");
        QTRY_VERIFY_WITH_TIMEOUT(!Root()->property("listsHidden").toBool(),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Shown("list") && Shown("list2"), TIMEOUT_MS);
        QVERIFY(!Shell_->CurrentLayout().ListsHidden);
    }

    void CommandsOpensThePalette() {
        BootTwoPairs();
        if (QTest::currentTestFailed()) {
            return;
        }
        QObject* palette = Root()->findChild<QObject*>("commandPalette");
        QVERIFY(palette != nullptr);
        QVERIFY(!palette->property("opened").toBool());
        Click("terminal", "commandsButton");
        QTRY_VERIFY_WITH_TIMEOUT(palette->property("opened").toBool(),
                                 TIMEOUT_MS);
    }

    void TheTerminalKeepsTheKeyboardAfterAClickOnAButton() {
        BootTwoPairs();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenIdIn("list", "terminal", ALPHA);
        TerminalItem* const terminal = TerminalOf("terminal");
        terminal->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(terminal->hasActiveFocus(), TIMEOUT_MS);

        // The default focus policy of a Button on Linux takes the focus at a
        // click; on macOS it does not, so the policy is checked by itself too.
        for (const char* name : {"listsButton", "commandsButton"}) {
            QTRY_VERIFY_WITH_TIMEOUT(HeadItem("terminal", name) != nullptr,
                                     TIMEOUT_MS);
            QCOMPARE(
                HeadItem("terminal", name)->property("focusPolicy").toInt(),
                static_cast<int>(Qt::NoFocus));
        }

        Click("terminal", "listsButton");
        QTRY_VERIFY_WITH_TIMEOUT(Root()->property("listsHidden").toBool(),
                                 TIMEOUT_MS);
        QVERIFY(terminal->hasActiveFocus());

        // The keys still go to the session.
        for (const char key : {'w', 'o', 'r', 'l', 'd'}) {
            QTest::keyClick(Window(), key);
            // Each key in a read of its own: the split a slow runner makes.
            QTest::qWait(50);
        }
        QTRY_VERIFY2_WITH_TIMEOUT(ScreenOf("terminal").contains(TYPED_WORLD),
                                  qPrintable(ScreenReport("terminal")),
                                  TIMEOUT_MS);
    }
};

QTEST_MAIN(TerminalHeadTest)
#include "terminal_head_test.moc"
