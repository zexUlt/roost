// Reset the layout (`layout.reset`) and undo a layout edit (`layout.undo`).
// The first half is the pure parts on trees (no window): the default layout
// for the panels there are, and the bounded history of the documents. The
// second half is the real window against a real roostd: the confirmation
// dialog, the menu items and the commands, one write of the layout per edit,
// several undos in a row, and the layout that comes back after a restart.

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlExtensionPlugin>
#include <QSignalSpy>
#include <QTest>

#include <optional>

#include "helpers/layout_window.hpp"
#include "helpers/scoped_env.hpp"
#include "roost/layout_commands.hpp"

using namespace roost;
using namespace roost::testing;

Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

LayoutKnownPanels Known() {
    return {
        {"sessions.list",
         {.Inputs = {}, .Outputs = {{"selected", "SessionRef"}}}},
        {"terminal.view",
         {.Inputs = {{"session", "SessionRef"}}, .Outputs = {}}},
    };
}

const QStringList ORDER_OF_TYPES = {"sessions.list", "terminal.view"};

LayoutNode Leaf(const QString& instance, const QString& panel,
                const QMap<QString, LayoutLink>& links = {}) {
    LayoutNode node;
    node.Instance = instance;
    node.Panel = panel;
    node.Links = links;
    return node;
}

LayoutLink From(const QString& from) {
    return {.From = from, .Output = "selected", .Problem = {}};
}

LayoutNode Split(bool horizontal, std::vector<LayoutNode> children,
                 std::vector<LayoutSize> sizes) {
    LayoutNode node;
    node.Kind = ELayoutNodeKind::Split;
    node.Horizontal = horizontal;
    node.Children = std::move(children);
    node.Sizes = std::move(sizes);
    return node;
}

// The list on the right: what "Move out of tabs" made before #223.
Layout Reversed() {
    return {.Root = Split(
                true,
                {Leaf("terminal", "terminal.view", {{"session", From("list")}}),
                 Leaf("list", "sessions.list")},
                {LayoutSize::Rest(), LayoutSize::Px(320)})};
}

// The default and a panel that it has not: its place, links and size go.
Layout WithAnExtraPanel() {
    return {.Root = Split(true,
                          {Leaf("list", "sessions.list"),
                           Split(false,
                                 {Leaf("terminal", "terminal.view",
                                       {{"session", From("list")}}),
                                  Leaf("terminal-2", "terminal.view",
                                       {{"session", From("list")}})},
                                 {LayoutSize::Px(200), LayoutSize::Rest()})},
                          {LayoutSize::Px(500), LayoutSize::Rest()})};
}

Layout Numbered(int n) {
    Layout layout = DefaultLayout();
    layout.Root.Children[0].Instance = QString("list-%1").arg(n);
    return layout;
}

}  // namespace

class LayoutResetUndoTest : public LayoutWindow {
    Q_OBJECT

private:
    std::optional<ScopedEnv> Home_;
    std::optional<ScopedEnv> Socket_;
    std::optional<ScopedEnv> Config_;

    // The writes of the layout document the window has seen since the spy was
    // made: each is one write of roostd.
    static int Writes(const QSignalSpy& spy) {
        int count = 0;
        for (const auto& call : spy) {
            if (call.at(0).toString() == QLatin1String("layout")) {
                ++count;
            }
        }
        return count;
    }

    void Focus(const QString& instance) {
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance(instance) != nullptr,
                                 TIMEOUT_MS);
        QQuickItem* slot = SlotByInstance(instance);
        slot->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(slot->hasActiveFocus(), TIMEOUT_MS);
    }

    // Runs the command with the keyboard in `instance`; roostd writes the
    // layout once, and what it keeps is what the window shows.
    void RunWritesOnce(const QString& instance, const QString& command) {
        Focus(instance);
        QVERIFY(Api_->Commands().HasLocal(command));
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Api_->Commands().Run(command);
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        // The write is the one this command made, not a later one: the
        // document is the layout of the window, and nothing follows.
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredLayout(), LayoutToJson(Shell_->CurrentLayout()), TIMEOUT_MS);
        QCOMPARE(Writes(spy), 1);
    }

    bool HasProblem(const QString& part) {
        for (const QString& problem : Problems()) {
            if (problem.contains(part)) {
                return true;
            }
        }
        return false;
    }

    static QQuickItem* InItems(QQuickItem* item, const QString& name) {
        if (item->objectName() == name) {
            return item;
        }
        for (QQuickItem* child : item->childItems()) {
            if (QQuickItem* found = InItems(child, name)) {
                return found;
            }
        }
        return nullptr;
    }

    // An item of the window by objectName, also in a dialog, which hangs on
    // the overlay of the window.
    QQuickItem* Named(const QString& name) {
        if (QQuickItem* found = InItems(Window()->contentItem(), name)) {
            return found;
        }
        return Root()->findChild<QQuickItem*>(name);
    }

    QObject* Dialog() { return Root()->findChild<QObject*>("confirmDialog"); }

    bool DialogOpen() {
        return Dialog() != nullptr && Dialog()->property("visible").toBool();
    }

    static QPoint CenterOf(QQuickItem* item) {
        return item->mapToScene(QPointF(item->width() / 2, item->height() / 2))
            .toPoint();
    }

    void WaitForDialog() {
        QTRY_VERIFY_WITH_TIMEOUT(DialogOpen(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Named("confirmButton") != nullptr &&
                                     Named("cancelButton") != nullptr &&
                                     Named("confirmButton")->isVisible(),
                                 TIMEOUT_MS);
    }

    void ClickIn(const QString& name) {
        QQuickItem* button = Named(name);
        QVERIFY(button != nullptr);
        QTest::mouseClick(Window(), Qt::LeftButton, Qt::NoModifier,
                          CenterOf(button));
    }

    // The menu of the panel, opened with the right button on it.
    void OpenMenuOn(const QString& instance) {
        // The menu that was open is closed (a closing one takes no click).
        if (QObject* open = Root()->findChild<QObject*>("panelMenu")) {
            QTRY_VERIFY_WITH_TIMEOUT(!open->property("visible").toBool(),
                                     TIMEOUT_MS);
        }
        QQuickItem* slot = SlotByInstance(instance);
        QVERIFY(slot != nullptr);
        QTest::mouseMove(Window(), slot->mapToScene(QPointF(40, 40)).toPoint());
        QTest::mouseClick(Window(), Qt::RightButton, Qt::NoModifier,
                          slot->mapToScene(QPointF(40, 40)).toPoint());
        QObject* popup = Root()->findChild<QObject*>("panelMenu");
        QVERIFY(popup != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(popup->property("visible").toBool(),
                                 TIMEOUT_MS);
    }

    // The layout is set, the window shows its panels and the history is empty
    // of what the set did (SetLayout is no edit).
    void StartWith(const Layout& layout) {
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Shell_->SetLayout(layout);
        QTRY_COMPARE_WITH_TIMEOUT(Shell_->CurrentLayout(), layout, TIMEOUT_MS);
        // The write is seen by the window: it is not a late one of a test.
        QTRY_VERIFY_WITH_TIMEOUT(Writes(spy) >= 1, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(StoredLayout(), LayoutToJson(layout),
                                  TIMEOUT_MS);
    }

    // Asks for the reset with the command and accepts the question.
    void ResetAccepted(const QString& instance) {
        Focus(instance);
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Api_->Commands().Run("layout.reset");
        WaitForDialog();
        ClickIn("confirmButton");
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!DialogOpen(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredLayout(), LayoutToJson(Shell_->CurrentLayout()), TIMEOUT_MS);
    }

    // The panel has the keyboard, and keeps it: the window does not move it
    // somewhere later.
    void VerifyKeyboardIn(const QString& instance) {
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance(instance) != nullptr &&
                                     SlotByInstance(instance)->hasActiveFocus(),
                                 TIMEOUT_MS);
        QTest::qWait(300);
        QVERIFY(SlotByInstance(instance) != nullptr);
        QVERIFY(SlotByInstance(instance)->hasActiveFocus());
    }

    // Runs the command (the keyboard is where the last step left it) and
    // waits for the one write of the layout it makes.
    void RunHere(const QString& command) {
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Api_->Commands().Run(command);
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredLayout(), LayoutToJson(Shell_->CurrentLayout()), TIMEOUT_MS);
    }

    // Accepts the question of the reset that is open; no write is expected.
    void AcceptOpenQuestion() {
        WaitForDialog();
        ClickIn("confirmButton");
        QTRY_VERIFY_WITH_TIMEOUT(!DialogOpen(), TIMEOUT_MS);
        QTest::qWait(200);
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

    // ---- the pure parts ---------------------------------------------------

    void ResetOfAReversedLayoutIsTheDefault() {
        const LayoutEdit edit =
            ResetLayout(Reversed(), ORDER_OF_TYPES, Known());
        QVERIFY(edit.Refused.isEmpty());
        QVERIFY(edit.Changed);
        QCOMPARE(edit.Tree, DefaultLayout());
        // The list is the first child: on the left.
        QCOMPARE(edit.Tree.Root.Children[0].Panel, QString("sessions.list"));
    }

    void ResetKeepsTheInstancesOfThePanelsOfTheDefaultAndDropsTheRest() {
        const LayoutEdit edit =
            ResetLayout(WithAnExtraPanel(), ORDER_OF_TYPES, Known());
        QVERIFY(edit.Changed);
        QCOMPARE(edit.Tree, DefaultLayout());
        QVERIFY(FindLeaf(edit.Tree.Root, "list") != nullptr);
        QVERIFY(FindLeaf(edit.Tree.Root, "terminal") != nullptr);
        QVERIFY(FindLeaf(edit.Tree.Root, "terminal-2") == nullptr);
        QCOMPARE(CountLeaves(edit.Tree.Root), 2);
        // The terminal is fed by the list, as in the default.
        QCOMPARE(FindLeaf(edit.Tree.Root, "terminal")->Links.value("session"),
                 From("list"));
    }

    void ResetClearsTheHiddenListsAndLeavesACanonicalTree() {
        Layout hidden = DefaultLayout();
        hidden.ListsHidden = true;
        const LayoutEdit edit = ResetLayout(hidden, ORDER_OF_TYPES, Known());
        QVERIFY(edit.Changed);
        QVERIFY(!edit.Tree.ListsHidden);
        // The canonical tree reads back as itself, with no problem.
        const LayoutParseResult parsed =
            ParseLayout(LayoutToJson(edit.Tree), Known());
        QVERIFY2(parsed.Problems.isEmpty(),
                 qPrintable(parsed.Problems.join("; ")));
        QCOMPARE(parsed.Tree, edit.Tree);
    }

    void ResetOfTheDefaultIsNeitherARefusalNorAChange() {
        const LayoutEdit edit =
            ResetLayout(DefaultLayout(), ORDER_OF_TYPES, Known());
        QVERIFY(edit.Refused.isEmpty());
        QVERIFY(!edit.Changed);
        QCOMPARE(edit.Tree, DefaultLayout());
    }

    void TheHistoryKeepsTheLastTwentyDocumentsNewestFirstOut() {
        LayoutHistory history;
        QVERIFY(!PopLayoutHistory(history).has_value());
        for (int n = 1; n <= 25; ++n) {
            PushLayoutHistory(history, Numbered(n));
        }
        QCOMPARE(history.size(), LAYOUT_HISTORY_DEPTH);
        QCOMPARE(LAYOUT_HISTORY_DEPTH, std::size_t{20});
        // The newest first; the five oldest are gone.
        for (int n = 25; n >= 6; --n) {
            const std::optional<Layout> last = PopLayoutHistory(history);
            QVERIFY(last.has_value());
            QCOMPARE(*last, Numbered(n));
        }
        QVERIFY(!PopLayoutHistory(history).has_value());
    }

    // ---- the window -------------------------------------------------------

    void TheCommandAsksFirstAndTheAnswerYesResetsTheLayout() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        StartWith(Reversed());
        ResetAccepted("list");
        QCOMPARE(Shell_->CurrentLayout(), DefaultLayout());
        // The list is on the left of the terminal in the window.
        QTRY_VERIFY_WITH_TIMEOUT(CenterOf(SlotByInstance("list")).x() <
                                     CenterOf(SlotByInstance("terminal")).x(),
                                 TIMEOUT_MS);
        QVERIFY(Problems().isEmpty());
    }

    void TheDialogShowsTheQuestionAndBothButtons() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        StartWith(Reversed());
        Focus("list");
        Api_->Commands().Run("layout.reset");
        WaitForDialog();
        const QRectF window(0, 0, Window()->width(), Window()->height());
        for (const char* name :
             {"confirmTitle", "confirmText", "confirmButton", "cancelButton"}) {
            QQuickItem* item = Named(name);
            QVERIFY2(item != nullptr, name);
            QVERIFY2(item->isVisible(), name);
            const QRectF at = item->mapRectToScene(
                QRectF(0, 0, item->width(), item->height()));
            QVERIFY2(window.contains(at), name);
        }
        QCOMPARE(Named("confirmTitle")->property("text").toString(),
                 QString("Reset the layout to the default?"));
        QVERIFY(Named("confirmText")
                    ->property("text")
                    .toString()
                    .contains("default"));
        QCOMPARE(Named("confirmButton")->property("text").toString(),
                 QString("Reset"));
        QCOMPARE(Named("cancelButton")->property("text").toString(),
                 QString("Cancel"));
        // Nothing is changed while the question is open.
        QCOMPARE(Shell_->CurrentLayout(), Reversed());
    }

    void CancelAndEscapeDeclineAndChangeNothing() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        StartWith(Reversed());
        Focus("list");
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);

        Api_->Commands().Run("layout.reset");
        WaitForDialog();
        ClickIn("cancelButton");
        QTRY_VERIFY_WITH_TIMEOUT(!DialogOpen(), TIMEOUT_MS);

        Api_->Commands().Run("layout.reset");
        WaitForDialog();
        QTest::keyClick(Window(), Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!DialogOpen(), TIMEOUT_MS);

        QTest::qWait(200);
        QCOMPARE(Writes(spy), 0);
        QCOMPARE(Shell_->CurrentLayout(), Reversed());
        QCOMPARE(StoredLayout(), LayoutToJson(Reversed()));
        // Nothing was edited: there is nothing to undo.
        Api_->Commands().Run("layout.undo");
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem("nothing to undo"), TIMEOUT_MS);
    }

    void TheMenuItemAsksAndResets() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        StartWith(WithAnExtraPanel());
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal-2") != nullptr,
                                 TIMEOUT_MS);
        OpenMenuOn("terminal-2");
        QQuickItem* reset = Named("menuReset");
        QVERIFY(reset != nullptr);
        QVERIFY(reset->isEnabled());
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        QTest::mouseClick(Window(), Qt::LeftButton, Qt::NoModifier,
                          CenterOf(reset));
        WaitForDialog();
        QCOMPARE(Writes(spy), 0);
        ClickIn("confirmButton");
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        QCOMPARE(Shell_->CurrentLayout(), DefaultLayout());
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal-2") == nullptr,
                                 TIMEOUT_MS);
        QCOMPARE(StoredLayout(), LayoutToJson(DefaultLayout()));
    }

    void TheResetLayoutIsStillThereAfterARestart() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        StartWith(Reversed());
        ResetAccepted("list");

        CloseWindow();
        StopDaemon();
        QVERIFY(StartDaemon());
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(Shell_->CurrentLayout(), DefaultLayout(),
                                  TIMEOUT_MS);
        QCOMPARE(StoredLayout(), LayoutToJson(DefaultLayout()));
        QVERIFY(Problems().isEmpty());
        // The history is memory: a new window has nothing to undo.
        Focus("list");
        Api_->Commands().Run("layout.undo");
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem("nothing to undo"), TIMEOUT_MS);
    }

    // Every kind of edit, then all of them undone one by one: each undo gives
    // the document that was before that edit, as one write.
    void EachEditIsUndoneOneStepAtATime() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        std::vector<Layout> before;
        const auto edit = [&](const QString& instance, const QString& command) {
            before.push_back(Shell_->CurrentLayout());
            RunWritesOnce(instance, command);
            QVERIFY(Shell_->CurrentLayout() != before.back());
        };
        edit("terminal", "layout.splitRight");
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal-2") != nullptr,
                                 TIMEOUT_MS);
        edit("terminal-2", "layout.intoTabs");
        edit("terminal-2", "layout.outOfTabs");
        // A link is removed, as the link editor does it.
        before.push_back(Shell_->CurrentLayout());
        {
            Focus("terminal");
            QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
            const QVariant args = QVariantMap{{"instance", "terminal"},
                                              {"input", "session"},
                                              {"from", ""},
                                              {"output", ""}};
            QVERIFY(QMetaObject::invokeMethod(Root(), "layoutEdit",
                                              Q_ARG(QString, "setLink"),
                                              Q_ARG(QVariant, args)));
            QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        }
        edit("list", "layout.toggleLists");
        edit("terminal-2", "layout.close");
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal-2") == nullptr,
                                 TIMEOUT_MS);
        before.push_back(Shell_->CurrentLayout());
        ResetAccepted("terminal");
        QCOMPARE(Shell_->CurrentLayout(), DefaultLayout());

        for (auto at = before.rbegin(); at != before.rend(); ++at) {
            RunWritesOnce("terminal", "layout.undo");
            QCOMPARE(Shell_->CurrentLayout(), *at);
            QCOMPARE(StoredLayout(), LayoutToJson(*at));
        }
        QVERIFY(Problems().isEmpty());
        // All of them are undone: the next undo is refused.
        Api_->Commands().Run("layout.undo");
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem("nothing to undo"), TIMEOUT_MS);
    }

    void UndoAfterCloseBringsThePanelBackWithItsLinks() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        const Layout before = Shell_->CurrentLayout();
        RunWritesOnce("terminal", "layout.close");
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal") == nullptr,
                                 TIMEOUT_MS);
        QCOMPARE(CountLeaves(Shell_->CurrentLayout().Root), 1);

        RunWritesOnce("list", "layout.undo");
        QCOMPARE(Shell_->CurrentLayout(), before);
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("terminal") != nullptr,
                                 TIMEOUT_MS);
        const LayoutNode* terminal =
            FindLeaf(Shell_->CurrentLayout().Root, "terminal");
        QVERIFY(terminal != nullptr);
        QCOMPARE(terminal->Links.value("session"), From("list"));

        // And the other way: the list closed, the terminal loses its link,
        // and the undo gives it back.
        RunWritesOnce("terminal", "layout.close");
        QCOMPARE(CountLeaves(Shell_->CurrentLayout().Root), 1);
        RunWritesOnce("list", "layout.undo");
        QCOMPARE(Shell_->CurrentLayout(), before);
    }

    void TheMenuItemUndoesAndIsOffWithNothingToUndo() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        OpenMenuOn("terminal");
        QQuickItem* undo = Named("menuUndo");
        QVERIFY(undo != nullptr);
        QVERIFY(!undo->isEnabled());
        QTest::keyClick(Window(), Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!Root()
                                      ->findChild<QObject*>("panelMenu")
                                      ->property("visible")
                                      .toBool(),
                                 TIMEOUT_MS);

        const Layout start = Shell_->CurrentLayout();
        RunWritesOnce("terminal", "layout.splitRight");
        // The new panel stands to the right of the source, laid out.
        QTRY_VERIFY_WITH_TIMEOUT(
            SlotByInstance("terminal-2") != nullptr &&
                SlotByInstance("terminal-2")->width() > 0 &&
                CenterOf(SlotByInstance("terminal-2")).x() >
                    CenterOf(SlotByInstance("terminal")).x(),
            TIMEOUT_MS);
        OpenMenuOn("terminal");
        undo = Named("menuUndo");
        QTRY_VERIFY_WITH_TIMEOUT(undo->isEnabled(), TIMEOUT_MS);
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        QTest::mouseClick(Window(), Qt::LeftButton, Qt::NoModifier,
                          CenterOf(undo));
        QTRY_COMPARE_WITH_TIMEOUT(Writes(spy), 1, TIMEOUT_MS);
        QCOMPARE(Shell_->CurrentLayout(), start);
        QCOMPARE(StoredLayout(), LayoutToJson(start));
        // Nothing is left to undo: the item is off again.
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal-2") == nullptr,
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal")->width() > 590,
                                 TIMEOUT_MS);
        OpenMenuOn("terminal");
        QTRY_VERIFY_WITH_TIMEOUT(!Named("menuUndo")->isEnabled(), TIMEOUT_MS);
    }

    void UndoWithNoHistoryIsRefusedAndTheStripSaysWhy() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        Focus("terminal");
        QSignalSpy spy(&Api_->Store(), &api::StoreService::Changed);
        Api_->Commands().Run("layout.undo");
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem("nothing to undo"), TIMEOUT_MS);
        QTest::qWait(200);
        QCOMPARE(Writes(spy), 0);
        QCOMPARE(Shell_->CurrentLayout(), DefaultLayout());
        // The next edit that works clears the reason.
        Api_->Commands().Run("layout.splitRight");
        QTRY_VERIFY_WITH_TIMEOUT(!HasProblem("nothing to undo"), TIMEOUT_MS);
    }

    void TheHistoryHoldsTwentyEditsAndTheOldestIsForgotten() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        const Layout first = Shell_->CurrentLayout();
        // The lists hidden and shown again, 21 times: each is an edit.
        for (int n = 0; n < 21; ++n) {
            RunWritesOnce("terminal", "layout.toggleLists");
        }
        for (int n = 0; n < 20; ++n) {
            RunWritesOnce("terminal", "layout.undo");
        }
        // The 21st edit is forgotten: the layout is the one after the first
        // toggle, not the first one.
        QVERIFY(Shell_->CurrentLayout() != first);
        QVERIFY(Shell_->CurrentLayout().ListsHidden);
        Api_->Commands().Run("layout.undo");
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem("nothing to undo"), TIMEOUT_MS);
    }

    void TheKeyboardIsInTheTerminalAfterUndoOfTheHiddenLists() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        RunWritesOnce("terminal", "layout.toggleLists");
        VerifyKeyboardIn("terminal");
        RunHere("layout.undo");
        QVERIFY(!Shell_->CurrentLayout().ListsHidden);
        VerifyKeyboardIn("terminal");
    }

    void TheKeyboardIsInTheSplitPanelAfterUndoOfTheSplit() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        Focus("list");
        const QVariant args = QVariantMap{{"instance", "terminal"}};
        QVERIFY(QMetaObject::invokeMethod(Root(), "layoutEdit",
                                          Q_ARG(QString, "splitRight"),
                                          Q_ARG(QVariant, args)));
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal-2") != nullptr,
                                 TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredLayout(), LayoutToJson(Shell_->CurrentLayout()), TIMEOUT_MS);
        // The edit moved the keyboard from the list to the panel it split.
        VerifyKeyboardIn("terminal");
        RunHere("layout.undo");
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal-2") == nullptr,
                                 TIMEOUT_MS);
        VerifyKeyboardIn("terminal");
    }

    void TheKeyboardIsInAPanelAfterTheResetOfTheCommand() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        StartWith(WithAnExtraPanel());
        // The panel that has the keyboard stays in the default: it keeps it.
        ResetAccepted("terminal");
        VerifyKeyboardIn("terminal");
        StartWith(WithAnExtraPanel());
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal-2") != nullptr,
                                 TIMEOUT_MS);
        // The one that has it is not in the default: the first panel gets it.
        ResetAccepted("terminal-2");
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal-2") == nullptr,
                                 TIMEOUT_MS);
        VerifyKeyboardIn("list");
    }

    void TheDialogOpensWithTheKeyboardOnCancelFromTheCommandAndTheMenu() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        StartWith(Reversed());
        Focus("list");
        Api_->Commands().Run("layout.reset");
        WaitForDialog();
        QTRY_VERIFY_WITH_TIMEOUT(Named("cancelButton")->hasActiveFocus(),
                                 TIMEOUT_MS);
        ClickIn("cancelButton");
        QTRY_VERIFY_WITH_TIMEOUT(!DialogOpen(), TIMEOUT_MS);

        OpenMenuOn("terminal");
        QQuickItem* reset = Named("menuReset");
        QVERIFY(reset != nullptr);
        QTest::mouseClick(Window(), Qt::LeftButton, Qt::NoModifier,
                          CenterOf(reset));
        WaitForDialog();
        QTRY_VERIFY_WITH_TIMEOUT(Named("cancelButton")->hasActiveFocus(),
                                 TIMEOUT_MS);
        ClickIn("cancelButton");
        QTRY_VERIFY_WITH_TIMEOUT(!DialogOpen(), TIMEOUT_MS);
    }

    void ResetOfTheDefaultLayoutLeavesNothingToUndo() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        QCOMPARE(Shell_->CurrentLayout(), DefaultLayout());
        QVERIFY(!Root()->property("canUndo").toBool());
        Focus("terminal");
        Api_->Commands().Run("layout.reset");
        AcceptOpenQuestion();
        QVERIFY(!Root()->property("canUndo").toBool());
        Api_->Commands().Run("layout.undo");
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem("nothing to undo"), TIMEOUT_MS);
        QCOMPARE(Shell_->CurrentLayout(), DefaultLayout());
    }

    void ResetOfTheDefaultAfterAnEditAndItsUndoAddsNoStep() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        WaitForLayout({{"list", "terminal"}});
        RunWritesOnce("terminal", "layout.toggleLists");
        RunWritesOnce("terminal", "layout.undo");
        QCOMPARE(Shell_->CurrentLayout(), DefaultLayout());
        QTRY_VERIFY_WITH_TIMEOUT(!Root()->property("canUndo").toBool(),
                                 TIMEOUT_MS);
        Focus("terminal");
        Api_->Commands().Run("layout.reset");
        AcceptOpenQuestion();
        QVERIFY(!Root()->property("canUndo").toBool());
        Api_->Commands().Run("layout.undo");
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem("nothing to undo"), TIMEOUT_MS);
    }
};

QTEST_MAIN(LayoutResetUndoTest)
#include "layout_reset_undo_test.moc"
