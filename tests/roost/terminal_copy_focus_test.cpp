// terminal.copy with two terminal panels in the window: the command runs in
// the panel that has the focus, else in the one that had it last, not in the
// one registered last. Real roostd, two pairs of a list and a terminal, each
// terminal showing another session; the selection is made with the mouse, the
// command is run as a key would run it (CommandService::Run).

#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QQmlExtensionPlugin>
#include <QTest>

#include <optional>

#include "helpers/layout_window.hpp"
#include "helpers/scoped_env.hpp"

using namespace roost;
using namespace roost::testing;

Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

const QString KEPT = QStringLiteral("kept");

}  // namespace

class TerminalCopyFocusTest : public LayoutWindow {
    Q_OBJECT

private:
    // The first row of the terminal, from its left edge across the greeting,
    // selected with the mouse; a press takes the focus.
    void SelectRowZero(TerminalItem* terminal) {
        const qreal y = terminal->cellHeight() / 2;
        const QPoint from = terminal->mapToScene(QPointF(2, y)).toPoint();
        const QPoint to =
            terminal->mapToScene(QPointF(terminal->cellWidth() * 40, y))
                .toPoint();
        QTest::mousePress(Window(), Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(Window(), to);
        QTest::mouseRelease(Window(), Qt::LeftButton, Qt::NoModifier, to);
        QVERIFY(terminal->hasSelection());
    }

    // Runs the command and waits for its answer.
    void Copy() {
        QGuiApplication::clipboard()->setText(KEPT);
        const QFuture<rpc::Result> done = Api_->Commands().Run("terminal.copy");
        QTRY_VERIFY_WITH_TIMEOUT(done.isFinished(), TIMEOUT_MS);
        QVERIFY(done.result());
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

    void CopyRunsInThePanelThatHasTheFocus() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Shell_->SetLayout(
            Stacked({Pair("list", "terminal"), Pair("list2", "terminal2")}));
        WaitForLayout({{"list", "terminal"}, {"list2", "terminal2"}});
        const QString idFirst = IdIn("list", 0);
        const QString idSecond = IdIn("list2", 1);
        QVERIFY(idFirst != idSecond);
        OpenIdIn("list", "terminal", idFirst);
        OpenIdIn("list2", "terminal2", idSecond);
        // The greeting is on the screen of each.
        TerminalItem* const first = TerminalOf("terminal");
        TerminalItem* const second = TerminalOf("terminal2");
        QTRY_VERIFY2_WITH_TIMEOUT(
            ScreenOf("terminal").contains(Greeting(idFirst)),
            qPrintable(ScreenReport("terminal")), TIMEOUT_MS);
        QTRY_VERIFY2_WITH_TIMEOUT(
            ScreenOf("terminal2").contains(Greeting(idSecond)),
            qPrintable(ScreenReport("terminal2")), TIMEOUT_MS);

        SelectRowZero(first);
        SelectRowZero(second);
        QVERIFY(first->hasSelection());
        QVERIFY(second->hasSelection());

        // The second registered last; the focus decides.
        first->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(first->hasActiveFocus(), TIMEOUT_MS);
        Copy();
        QVERIFY(QGuiApplication::clipboard()->text().contains(idFirst));
        QVERIFY(!QGuiApplication::clipboard()->text().contains(idSecond));

        second->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(second->hasActiveFocus(), TIMEOUT_MS);
        Copy();
        QVERIFY(QGuiApplication::clipboard()->text().contains(idSecond));
        QVERIFY(!QGuiApplication::clipboard()->text().contains(idFirst));
    }

    // The focus is in no terminal: it went to a list.
    void WithoutTheFocusInAnyTerminalTheOneThatHadItLastRuns() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Shell_->SetLayout(
            Stacked({Pair("list", "terminal"), Pair("list2", "terminal2")}));
        WaitForLayout({{"list", "terminal"}, {"list2", "terminal2"}});
        const QString idFirst = IdIn("list", 0);
        const QString idSecond = IdIn("list2", 1);
        QVERIFY(idFirst != idSecond);
        OpenIdIn("list", "terminal", idFirst);
        OpenIdIn("list2", "terminal2", idSecond);
        TerminalItem* const first = TerminalOf("terminal");
        TerminalItem* const second = TerminalOf("terminal2");
        QTRY_VERIFY2_WITH_TIMEOUT(
            ScreenOf("terminal").contains(Greeting(idFirst)),
            qPrintable(ScreenReport("terminal")), TIMEOUT_MS);
        QTRY_VERIFY2_WITH_TIMEOUT(
            ScreenOf("terminal2").contains(Greeting(idSecond)),
            qPrintable(ScreenReport("terminal2")), TIMEOUT_MS);
        SelectRowZero(first);
        SelectRowZero(second);

        // The human worked in the first terminal, then went to a list: the
        // second one is the last registered, but not the one to copy from.
        first->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(first->hasActiveFocus(), TIMEOUT_MS);
        ListViewOf("list")->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(ListViewOf("list")->hasActiveFocus(),
                                 TIMEOUT_MS);
        QVERIFY(!first->hasActiveFocus());
        QVERIFY(!second->hasActiveFocus());
        Copy();
        QVERIFY(QGuiApplication::clipboard()->text().contains(idFirst));
        QVERIFY(!QGuiApplication::clipboard()->text().contains(idSecond));
    }
};

QTEST_MAIN(TerminalCopyFocusTest)
#include "terminal_copy_focus_test.moc"
