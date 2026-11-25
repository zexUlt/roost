// The key of a new session: Cmd+N on macOS, Ctrl+Shift+N elsewhere, the command
// `sessions.newSession` of the plugin `sessions`. It opens the form of the list
// of the keyboard (the list itself, the list that feeds the terminal that has
// it, or the first list), from anywhere in the window, and the terminal program
// gets nothing. The window, the daemon and the fake `claude` are the helpers of
// sessions_window.hpp and layout_window.hpp; the keys go through the window and
// the real keymap table of roostd. HOME, ROOST_SOCKET and ROOST_CONFIG are
// temporary.

#include <QDir>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QTest>

#include <optional>

#include "helpers/layout_window.hpp"
#include "helpers/scoped_env.hpp"

using namespace roost;
using namespace roost::testing;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

// The key as the manifest of `sessions` has it: Qt's Control is Command on
// macOS, where the key has no Shift.
#ifdef Q_OS_MACOS
constexpr Qt::KeyboardModifiers NEW_MODIFIERS = Qt::ControlModifier;
#else
constexpr Qt::KeyboardModifiers NEW_MODIFIERS =
    Qt::ControlModifier | Qt::ShiftModifier;
#endif

LayoutNode ListLeaf(const QString& instance) {
    LayoutNode node;
    node.Instance = instance;
    node.Panel = LIST;
    return node;
}

LayoutNode TerminalLeaf(const QString& instance, const QString& list) {
    LayoutNode node;
    node.Instance = instance;
    node.Panel = TERMINAL;
    node.Links.insert("session",
                      {.From = list, .Output = "selected", .Problem = {}});
    return node;
}

// A list, a terminal that is fed by the second list, and the second list: the
// terminal's list is not the first one.
Layout ListTerminalOfSecondList() {
    LayoutNode split;
    split.Kind = ELayoutNodeKind::Split;
    split.Horizontal = true;
    split.Children = {ListLeaf("list"), TerminalLeaf("terminal", "list2"),
                      ListLeaf("list2")};
    split.Sizes = {LayoutSize::Px(320), LayoutSize::Rest(),
                   LayoutSize::Px(320)};
    return {.Root = std::move(split)};
}

// A stack of the terminal fed by the list "hidden" and that list, the terminal
// shown, and the list "visible" beside it: the linked list is on a tab that is
// not shown.
Layout TerminalOfListOnHiddenTab() {
    LayoutNode tabs;
    tabs.Kind = ELayoutNodeKind::Tabs;
    tabs.Active = 0;
    tabs.Children = {TerminalLeaf("terminal", "hidden"), ListLeaf("hidden")};
    LayoutNode split;
    split.Kind = ELayoutNodeKind::Split;
    split.Horizontal = true;
    split.Children = {std::move(tabs), ListLeaf("visible")};
    split.Sizes = {LayoutSize::Rest(), LayoutSize::Px(320)};
    return {.Root = std::move(split)};
}

}  // namespace

class SessionsNewKeyTest : public LayoutWindow {
    Q_OBJECT

private:
    std::optional<ScopedEnv> Home_;
    std::optional<ScopedEnv> Socket_;
    std::optional<ScopedEnv> Config_;

    bool FormOpenIn(const QString& list) {
        const QQuickItem* panel = PanelByInstance(list);
        return panel != nullptr && panel->property("newOpen").toBool();
    }

    QQuickItem* TaskOf(const QString& list) {
        return PanelByInstance(list)->findChild<QQuickItem*>("sessionsNewTask");
    }

    void Focus(const QString& instance) {
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance(instance) != nullptr,
                                 TIMEOUT_MS);
        QQuickItem* slot = SlotByInstance(instance);
        slot->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(slot->hasActiveFocus(), TIMEOUT_MS);
    }

    void BootWithTwoLists() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Shell_->SetLayout(ListTerminalOfSecondList());
        WaitForLayout({{"list2", "terminal"}}, false);
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("list") != nullptr,
                                 TIMEOUT_MS);
    }

    // The terminal with the linked list on a hidden tab and a list that is
    // shown. The hidden list is loaded, so a key that went to it would open its
    // form.
    void BootWithLinkedListOnHiddenTab() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Shell_->SetLayout(TerminalOfListOnHiddenTab());
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("visible") != nullptr &&
                                     PanelByInstance("hidden") != nullptr,
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal") != nullptr &&
                                     SlotByInstance("terminal")->isVisible(),
                                 TIMEOUT_MS);
        QVERIFY(!SlotByInstance("hidden")->isVisible());
        Focus("terminal");
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

    // The window has a handler for the command: the key is an entry of the
    // table and the list takes the form.
    void TheKeyInTheListOpensTheFormWithTheKeyboardInTask() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Api_->Commands().HasLocal("sessions.newSession"));
        QVERIFY(ListView()->hasActiveFocus());
        QVERIFY(!PanelOf(LIST)->property("newOpen").toBool());
        Press(Qt::Key_N, NEW_MODIFIERS);
        QTRY_VERIFY_WITH_TIMEOUT(PanelOf(LIST)->property("newOpen").toBool(),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(PanelOf(LIST)
                                     ->findChild<QQuickItem*>("sessionsNewTask")
                                     ->hasActiveFocus(),
                                 TIMEOUT_MS);
    }

    // From the terminal the key opens the form of the list that feeds it, and
    // the program gets no byte of it: the byte of a letter typed after the form
    // is the second the program has seen, not the third.
    void TheKeyInTheTerminalOpensTheFormAndSendsNoByteToTheProgram() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(Terminal()->hasActiveFocus(), TIMEOUT_MS);
        QTest::keyClick(Window(), 'w');
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains("input w total=1"),
                                 TIMEOUT_MS);

        Press(Qt::Key_N, NEW_MODIFIERS);

        QTRY_VERIFY_WITH_TIMEOUT(PanelOf(LIST)->property("newOpen").toBool(),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(PanelOf(LIST)
                                     ->findChild<QQuickItem*>("sessionsNewTask")
                                     ->hasActiveFocus(),
                                 TIMEOUT_MS);
        QVERIFY(!Terminal()->hasActiveFocus());
        // The barrier: back to the terminal and one more letter. Its screen
        // line counts the bytes the program has had.
        Press(Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);
        PressWhenSettled(Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(Terminal()->hasActiveFocus(), TIMEOUT_MS);
        QTest::keyClick(Window(), 'o');
        QTRY_VERIFY2_WITH_TIMEOUT(ScreenNow().contains("input o total=2"),
                                  qPrintable(ScreenNow()), TIMEOUT_MS);
    }

    // The terminal is fed by the second list: its form opens, not the first
    // list's, which would be the one of `shell.focusList` without links.
    void WithTwoListsTheLinkedOneOpens() {
        BootWithTwoLists();
        if (QTest::currentTestFailed()) {
            return;
        }
        Focus("terminal");
        Press(Qt::Key_N, NEW_MODIFIERS);
        QTRY_VERIFY_WITH_TIMEOUT(FormOpenIn("list2"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(TaskOf("list2")->hasActiveFocus(), TIMEOUT_MS);
        QVERIFY(!FormOpenIn("list"));
    }

    // A list that has the keyboard opens its own form, whatever the links.
    void WithTwoListsTheListThatHasTheKeyboardOpens() {
        BootWithTwoLists();
        if (QTest::currentTestFailed()) {
            return;
        }
        Focus("list");
        Press(Qt::Key_N, NEW_MODIFIERS);
        QTRY_VERIFY_WITH_TIMEOUT(FormOpenIn("list"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(TaskOf("list")->hasActiveFocus(), TIMEOUT_MS);
        QVERIFY(!FormOpenIn("list2"));
    }

    // The lists are hidden: there is no list to show the form in, and the key
    // does nothing (it does not show the lists either). The barrier is the
    // layout command that follows: it is answered after the key has been run.
    void WithTheListsHiddenTheKeyDoesNothing() {
        BootWithTwoLists();
        if (QTest::currentTestFailed()) {
            return;
        }
        Focus("terminal");
        Api_->Commands().Run("layout.toggleLists");
        QTRY_VERIFY_WITH_TIMEOUT(Root()->property("listsHidden").toBool(),
                                 TIMEOUT_MS);
        Press(Qt::Key_N, NEW_MODIFIERS);
        Api_->Commands().Run("layout.toggleLists");
        QTRY_VERIFY_WITH_TIMEOUT(!Root()->property("listsHidden").toBool(),
                                 TIMEOUT_MS);
        QVERIFY(!FormOpenIn("list"));
        QVERIFY(!FormOpenIn("list2"));
        // The key works again with the lists back.
        Focus("terminal");
        Press(Qt::Key_N, NEW_MODIFIERS);
        QTRY_VERIFY_WITH_TIMEOUT(FormOpenIn("list2"), TIMEOUT_MS);
    }

    // The list that feeds the terminal is on a tab that is not shown: the
    // keyboard cannot land in it, so the form opens in the first list that is
    // shown and the hidden list's state is left alone.
    void ALinkedListOnAHiddenTabIsSkipped() {
        BootWithLinkedListOnHiddenTab();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_N, NEW_MODIFIERS);
        QTRY_VERIFY_WITH_TIMEOUT(FormOpenIn("visible"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(TaskOf("visible")->hasActiveFocus(),
                                 TIMEOUT_MS);
        QVERIFY(!FormOpenIn("hidden"));
    }

    // `shell.focusList` goes by the same rule: the keyboard goes to the list
    // that is shown.
    void FocusListSkipsALinkedListOnAHiddenTab() {
        BootWithLinkedListOnHiddenTab();
        if (QTest::currentTestFailed()) {
            return;
        }
        Api_->Commands().Run("shell.focusList");
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("visible")->hasActiveFocus(),
                                 TIMEOUT_MS);
        QVERIFY(!SlotByInstance("hidden")->hasActiveFocus());
    }
};

QTEST_MAIN(SessionsNewKeyTest)
#include "sessions_new_key_test.moc"
