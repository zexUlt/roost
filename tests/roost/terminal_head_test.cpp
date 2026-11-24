// The head of the terminal panel (plugin `terminal`): the title and the meta
// line of the shown session, and the buttons "Lists" and "Commands". Real
// roostd with the real Claude plugin behind a fake `claude`, the real window;
// the buttons are clicked with the mouse through the window. The sessions
// change as the fake `claude` lists them anew (the file next to it, polled by
// the adapter), so that the head sees the changes of the real model.

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QSaveFile>
#include <QSignalSpy>
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

    // The head of the terminal is built and shows `title`.
    void WaitForTitle(const QString& terminal, const QString& title) {
        QTRY_VERIFY_WITH_TIMEOUT(HeadItem(terminal, "terminalTitle") != nullptr,
                                 TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(TitleOf(terminal), title, TIMEOUT_MS);
    }

    void WaitForMeta(const QString& terminal, const QString& meta) {
        QTRY_VERIFY_WITH_TIMEOUT(HeadItem(terminal, "terminalMeta") != nullptr,
                                 TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(MetaOf(terminal), meta, TIMEOUT_MS);
    }

    // `claude agents` lists the sessions of the fixture with `changes[id]`
    // laid over the entry of each session, and without those in `gone`; the
    // file is replaced whole.
    bool List(const QMap<QString, QJsonObject>& changes,
              const QStringList& gone = {}) const {
        QJsonArray listed;
        for (const QJsonValue& value :
             QJsonDocument::fromJson(
                 ReadFile(QStringLiteral(FIXTURES_DIR "/claude_agents.json")))
                 .array()) {
            QJsonObject entry = value.toObject();
            const QString id = entry.value("id").toString();
            if (gone.contains(id)) {
                continue;
            }
            const QJsonObject over = changes.value(id);
            for (auto change = over.begin(); change != over.end(); ++change) {
                entry.insert(change.key(), change.value());
            }
            listed.append(entry);
        }
        QSaveFile agents(AgentsPath());
        return agents.open(QIODevice::WriteOnly) &&
               agents.write(
                   QJsonDocument(listed).toJson(QJsonDocument::Compact)) >= 0 &&
               agents.commit();
    }

    // The title of the session in the model of the list, as the list has it.
    QString ListedTitle(const QString& list, const QString& id) {
        QAbstractItemModel* model = ModelOf(list);
        const int row = RowOfId(list, id);
        return model == nullptr || row < 0
                   ? QString()
                   : model
                         ->data(model->index(row, 0),
                                model->roleNames().key("title"))
                         .toString();
    }

    // The panel is `width` wide: the list of the pair takes the rest.
    void NarrowTo(int width) {
        const int now = static_cast<int>(SlotByInstance("terminal")->width());
        const int list = static_cast<int>(SlotByInstance("list")->width());
        Shell_->SetLayout(
            Stacked({Pair("list", "terminal", list + now - width)}));
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(SlotByInstance("terminal")->width() - width) < 2, TIMEOUT_MS);
    }

    // The size line of the fake TUI, which it redraws at SIGWINCH: the size
    // of the PTY, which the item says is its grid.
    QString PtySize(const QString& terminal) {
        const TerminalItem* item = TerminalOf(terminal);
        return QStringLiteral("size %1x%2")
            .arg(item->columns())
            .arg(item->rows());
    }

    void WriteSettings(const QJsonObject& settings) {
        const QFuture<rpc::Result> writing =
            Api_->Store().Set(QStringLiteral("settings"), settings);
        QTRY_VERIFY_WITH_TIMEOUT(writing.isFinished(), TIMEOUT_MS);
        QVERIFY(writing.result());
    }

    bool Visible(const QString& terminal, const char* name) {
        const QQuickItem* item = HeadItem(terminal, name);
        return item != nullptr && item->isVisible();
    }

    // Two terminal panels and no list: a click on a button of one hides
    // nothing, and "Lists" has nothing to hide.
    void BootTwoTerminals() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        LayoutNode first;
        first.Instance = "terminal";
        first.Panel = TERMINAL;
        LayoutNode second = first;
        second.Instance = "terminal2";
        Layout layout;
        layout.Root.Kind = ELayoutNodeKind::Split;
        layout.Root.Horizontal = false;
        layout.Root.Sizes = {LayoutSize::Rest(), LayoutSize::Rest()};
        layout.Root.Children = {first, second};
        Shell_->SetLayout(layout);
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("terminal") != nullptr &&
                                     PanelByInstance("terminal2") != nullptr &&
                                     PanelByInstance("list") == nullptr,
                                 TIMEOUT_MS);
        WaitForTitle("terminal", "No session");
        WaitForTitle("terminal2", "No session");
        TerminalOf("terminal")->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(TerminalOf("terminal")->hasActiveFocus(),
                                 TIMEOUT_MS);
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
        WaitForTitle("terminal", "No session");
        WaitForMeta("terminal", QString());
        QVERIFY(SaveSnapshot(Window(), "without-session"));

        OpenIdIn("list", "terminal", ALPHA);
        WaitForTitle("terminal", "Fix the flaky test");
        WaitForMeta("terminal", "working · /work/alpha");
        // The other terminal shows nothing yet.
        WaitForTitle("terminal2", "No session");
        QVERIFY(SaveSnapshot(Window(), "with-session"));

        // A session that waits is called so, as in the list.
        OpenIdIn("list", "terminal", BETA);
        WaitForTitle("terminal", "Review the PR");
        WaitForMeta("terminal", "waiting · /work/beta");
        WaitForTitle("terminal2", "No session");
    }

    // The head reads the real model: a rename, a change of state, a change
    // of another session, the session leaving the list and the list being
    // loaded again, in two panels that show the same session.
    void TheTitleFollowsTheModel() {
        BootTwoPairs();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenIdIn("list", "terminal", ALPHA);
        OpenIdIn("list2", "terminal2", ALPHA);
        const QStringList terminals = {"terminal", "terminal2"};
        for (const QString& terminal : terminals) {
            WaitForTitle(terminal, "Fix the flaky test");
            WaitForMeta(terminal, "working · /work/alpha");
        }

        // A rename and a new cwd: a change of one row.
        QJsonObject renamed{{"name", "Renamed"}, {"cwd", "/work/moved"}};
        QVERIFY(List({{ALPHA, renamed}}));
        for (const QString& terminal : terminals) {
            WaitForTitle(terminal, "Renamed");
            WaitForMeta(terminal, "working · /work/moved");
        }

        // A change of state, and the word the list says for it.
        QJsonObject blocked = renamed;
        blocked.insert("state", "blocked");
        QVERIFY(List({{ALPHA, blocked}}));
        for (const QString& terminal : terminals) {
            WaitForMeta(terminal, "waiting · /work/moved");
        }

        // A change of another session: the list has it, and the panel does
        // not take its `info` anew (the signal of a `var` comes at every
        // assignment, equal or not).
        QSignalSpy infoChanged(PanelByInstance("terminal"),
                               SIGNAL(infoChanged()));
        QVERIFY(List({{ALPHA, blocked}, {BETA, {{"name", "Another"}}}}));
        QTRY_COMPARE_WITH_TIMEOUT(ListedTitle("list", BETA), QString("Another"),
                                  TIMEOUT_MS);
        QCOMPARE(infoChanged.count(), 0);
        for (const QString& terminal : terminals) {
            QCOMPARE(TitleOf(terminal), QString("Renamed"));
            QCOMPARE(MetaOf(terminal), QString("waiting · /work/moved"));
        }
        // A change of the shown session is taken once.
        QVERIFY(List(
            {{ALPHA, {{"name", "Again"}}}, {BETA, {{"name", "Another"}}}}));
        WaitForTitle("terminal", "Again");
        QCOMPARE(infoChanged.count(), 1);

        // The session leaves the list: the list drops its choice and the
        // panels have no session.
        QVERIFY(List({}, {ALPHA}));
        for (const QString& terminal : terminals) {
            WaitForTitle(terminal, "No session");
            WaitForMeta(terminal, QString());
        }
    }

    // The model is replaced (the daemon goes and comes back) while the panel
    // keeps its session: the head has the id and no meta for as long as the
    // session is not listed, and the title when it is again.
    void TheHeadFallsBackToTheIdWhenTheListIsReloaded() {
        BootTwoPairs();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenIdIn("list", "terminal", ALPHA);
        WaitForTitle("terminal", "Fix the flaky test");
        WaitForMeta("terminal", "working · /work/alpha");

        StopDaemon();
        WaitForTitle("terminal", ALPHA);
        WaitForMeta("terminal", QString());

        QVERIFY(StartDaemon());
        WaitForTitle("terminal", "Fix the flaky test");
        WaitForMeta("terminal", "working · /work/alpha");
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

    // The keys still go to the session after a click on "Lists".
    void TheKeysReachTheSessionAfterAClickOnLists() {
        BootTwoPairs();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenIdIn("list", "terminal", ALPHA);
        TerminalItem* const terminal = TerminalOf("terminal");
        terminal->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(terminal->hasActiveFocus(), TIMEOUT_MS);

        Click("terminal", "listsButton");
        QTRY_VERIFY_WITH_TIMEOUT(Root()->property("listsHidden").toBool(),
                                 TIMEOUT_MS);
        QVERIFY(terminal->hasActiveFocus());

        // Each key in a read of its own: the next one goes after the screen
        // has shown the one before.
        int typed = 0;
        for (const char key : {'w', 'o', 'r', 'l', 'd'}) {
            QTest::keyClick(Window(), key);
            ++typed;
            const QRegularExpression total(
                QStringLiteral("total=%1(?!\\d)").arg(typed));
            QTRY_VERIFY2_WITH_TIMEOUT(ScreenOf("terminal").contains(total),
                                      qPrintable(ScreenReport("terminal")),
                                      TIMEOUT_MS);
        }
        QVERIFY2(ScreenOf("terminal").contains(TYPED_WORLD),
                 qPrintable(ScreenReport("terminal")));
    }

    // A click on a button of another terminal panel hides nothing and leaves
    // the keyboard where it was. The focus policy is checked by itself too:
    // the default one of a Button on Linux takes the focus at a click, on
    // macOS it does not.
    void AClickOnAButtonOfAnotherPanelLeavesTheKeyboardAlone() {
        BootTwoTerminals();
        if (QTest::currentTestFailed()) {
            return;
        }
        const TerminalItem* const first = TerminalOf("terminal");
        const TerminalItem* const second = TerminalOf("terminal2");
        // "Lists" has no list to hide: the click changes nothing.
        const Layout before = Shell_->CurrentLayout();
        Click("terminal2", "listsButton");
        // The barrier: the palette is opened by the next command, which comes
        // after the refused one.
        QObject* palette = Root()->findChild<QObject*>("commandPalette");
        QVERIFY(palette != nullptr);
        Click("terminal2", "commandsButton");
        QTRY_VERIFY_WITH_TIMEOUT(palette->property("opened").toBool(),
                                 TIMEOUT_MS);
        QVERIFY(Shell_->CurrentLayout() == before);
        QVERIFY(!Root()->property("listsHidden").toBool());
        QVERIFY(Shown("terminal") && Shown("terminal2"));

        // The palette closes, and the keyboard is back in the panel it came
        // from, not in the one whose button was clicked.
        Press(Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!palette->property("opened").toBool(),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(first->hasActiveFocus(), TIMEOUT_MS);
        QVERIFY(!second->hasActiveFocus());

        // The second line: the buttons take no focus by their policy.
        for (const char* name : {"listsButton", "commandsButton"}) {
            QCOMPARE(
                HeadItem("terminal2", name)->property("focusPolicy").toInt(),
                static_cast<int>(Qt::NoFocus));
        }
    }

    // The mouse on "Lists" of a window that has no list: refused, nothing
    // changes, the window and the panels live on and the stored layout has no
    // hidden state. That the hidden state survives a restart is the case of
    // layout_lists_test (TheHiddenStateSurvivesARestartOfTheWindowAndRoostd).
    void ListsInAWindowWithoutAListDoesNothing() {
        BootTwoTerminals();
        if (QTest::currentTestFailed()) {
            return;
        }
        const Layout before = Shell_->CurrentLayout();
        const int slotCount = SlotCount();
        for (int click = 0; click < 2; ++click) {
            Click("terminal", "listsButton");
        }
        // The barrier: a command that comes after the refused ones.
        QObject* palette = Root()->findChild<QObject*>("commandPalette");
        QVERIFY(palette != nullptr);
        Click("terminal", "commandsButton");
        QTRY_VERIFY_WITH_TIMEOUT(palette->property("opened").toBool(),
                                 TIMEOUT_MS);
        QVERIFY(Shell_->CurrentLayout() == before);
        QVERIFY(!Shell_->CurrentLayout().ListsHidden);
        QVERIFY(!Root()->property("listsHidden").toBool());
        QCOMPARE(SlotCount(), slotCount);
        QVERIFY(Shown("terminal") && Shown("terminal2"));
        QVERIFY(!StoredLayout().contains("listsHidden"));
        QCOMPARE(TitleOf("terminal"), QString("No session"));
    }

    // Markup and a line break in a title or a cwd are text on one line, as
    // the list shows them.
    void MarkupAndLineBreaksAreTextOnOneLine() {
        BootTwoPairs();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenIdIn("list", "terminal", ALPHA);
        QVERIFY(List(
            {{ALPHA,
              {{"name", "a <b>x</b>\nb"}, {"cwd", "/work/<i>c</i>\nd"}}}}));
        WaitForTitle("terminal", "a <b>x</b> b");
        WaitForMeta("terminal", "working · /work/<i>c</i> d");
        for (const char* name : {"terminalTitle", "terminalMeta"}) {
            const QQuickItem* text = HeadItem("terminal", name);
            QCOMPARE(text->property("textFormat").toInt(),
                     static_cast<int>(Qt::PlainText));
            QCOMPARE(text->property("lineCount").toInt(), 1);
        }
        QCOMPARE(HeadItem("terminal", "terminalHead")->height(), 76.0);
        QVERIFY(SaveSnapshot(Window(), "markup-title"));
    }

    // In a narrow panel the title keeps 80 px or more and is cut with an
    // ellipsis; "Commands" goes first and then "Lists"; the meta is cut in
    // the middle.
    void ANarrowPanelKeepsTheTitleAndDropsTheButtonsOneByOne() {
        BootTwoPairs();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(List({{ALPHA,
                       {{"name", "A title that is much too long for the head"},
                        {"cwd", "/work/some/very/deep/path/ending-here"}}}}));
        OpenIdIn("list", "terminal", ALPHA);
        WaitForTitle("terminal", "A title that is much too long for the head");
        // Wide: both buttons.
        QVERIFY(Visible("terminal", "listsButton"));
        QVERIFY(Visible("terminal", "commandsButton"));

        NarrowTo(260);
        QTRY_VERIFY_WITH_TIMEOUT(Visible("terminal", "listsButton") &&
                                     !Visible("terminal", "commandsButton"),
                                 TIMEOUT_MS);
        const QQuickItem* title = HeadItem("terminal", "terminalTitle");
        QVERIFY(title->isVisible());
        QVERIFY2(title->width() >= 80,
                 qPrintable(QString::number(title->width())));
        QVERIFY(title->property("truncated").toBool());
        QCOMPARE(
            HeadItem("terminal", "terminalMeta")->property("elide").toInt(),
            static_cast<int>(Qt::ElideMiddle));
        QVERIFY(SaveSnapshot(Window(), "narrow-260"));

        NarrowTo(160);
        QTRY_VERIFY_WITH_TIMEOUT(!Visible("terminal", "listsButton") &&
                                     !Visible("terminal", "commandsButton"),
                                 TIMEOUT_MS);
        QVERIFY2(title->width() >= 80,
                 qPrintable(QString::number(title->width())));
        QVERIFY(SaveSnapshot(Window(), "narrow-160"));

        // Wide again: they come back.
        NarrowTo(640);
        QTRY_VERIFY_WITH_TIMEOUT(Visible("terminal", "listsButton") &&
                                     Visible("terminal", "commandsButton"),
                                 TIMEOUT_MS);
    }

    // A large font of the interface: the two lines stay inside the head and
    // the terminal starts below it; the head is 76 again with the normal font.
    void ALargeInterfaceFontKeepsTheTextInsideTheHead() {
        BootTwoPairs();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenIdIn("list", "terminal", ALPHA);
        WaitForMeta("terminal", "working · /work/alpha");
        const QQuickItem* head = HeadItem("terminal", "terminalHead");
        const QQuickItem* heading = HeadItem("terminal", "terminalHeading");
        QVERIFY(head != nullptr && heading != nullptr);
        QCOMPARE(head->height(), 76.0);

        WriteSettings({{"uiFontSize", 36}});
        QTRY_VERIFY_WITH_TIMEOUT(head->height() > 76, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            heading->y() >= 0 &&
                heading->y() + heading->height() <= head->height() &&
                TerminalOf("terminal")->y() == head->height(),
            TIMEOUT_MS);
        QVERIFY(SaveSnapshot(Window(), "large-font"));

        WriteSettings({});
        QTRY_COMPARE_WITH_TIMEOUT(head->height(), 76.0, TIMEOUT_MS);
    }

    // The terminal lies under the head with no inset on top, the grid follows
    // the height it has, and the PTY follows the grid; the height is never
    // negative.
    void ThePtyFollowsTheHeightOfTheTerminalUnderTheHead() {
        BootTwoPairs();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenIdIn("list", "terminal", ALPHA);
        const QQuickItem* panel = PanelByInstance("terminal");
        const QQuickItem* head = HeadItem("terminal", "terminalHead");
        TerminalItem* const terminal = TerminalOf("terminal");
        const qreal inset =
            Singleton_->theme()->property("cornerInset").toReal();
        const auto lies = [&] {
            return terminal->y() == head->height() &&
                   terminal->height() ==
                       panel->height() - head->height() - inset &&
                   terminal->x() == inset;
        };
        QTRY_VERIFY_WITH_TIMEOUT(lies(), TIMEOUT_MS);
        QTRY_VERIFY2_WITH_TIMEOUT(
            ScreenOf("terminal").contains(PtySize("terminal")),
            qPrintable(ScreenReport("terminal")), TIMEOUT_MS);

        // The window grows: more rows, and the PTY has them.
        const int rows = terminal->rows();
        Window()->resize(Window()->width(), Window()->height() + 300);
        QTRY_VERIFY_WITH_TIMEOUT(terminal->rows() > rows, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(lies(), TIMEOUT_MS);
        QTRY_VERIFY2_WITH_TIMEOUT(
            ScreenOf("terminal").contains(PtySize("terminal")),
            qPrintable(ScreenReport("terminal")), TIMEOUT_MS);

        // The lists go: more columns, and the PTY has them.
        const int columns = terminal->columns();
        Click("terminal", "listsButton");
        QTRY_VERIFY_WITH_TIMEOUT(terminal->columns() > columns, TIMEOUT_MS);
        QTRY_VERIFY2_WITH_TIMEOUT(
            ScreenOf("terminal").contains(PtySize("terminal")),
            qPrintable(ScreenReport("terminal")), TIMEOUT_MS);
        Click("terminal", "listsButton");
        QTRY_COMPARE_WITH_TIMEOUT(terminal->columns(), columns, TIMEOUT_MS);
        QTRY_VERIFY2_WITH_TIMEOUT(
            ScreenOf("terminal").contains(PtySize("terminal")),
            qPrintable(ScreenReport("terminal")), TIMEOUT_MS);

        // A panel lower than the head: no negative height.
        Window()->resize(Window()->width(), 120);
        QTRY_VERIFY_WITH_TIMEOUT(panel->height() < head->height() + inset,
                                 TIMEOUT_MS);
        QVERIFY(terminal->height() >= 0);
    }
};

QTEST_MAIN(TerminalHeadTest)
#include "terminal_head_test.moc"
