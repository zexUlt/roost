// The commands of the window itself in the shell's window (Main.qml, the
// terminal panel), on the path a user's key takes: QTest::keyClick on the
// window goes through the windowing system interface, so Qt looks for a
// Shortcut (ShortcutOverride, QShortcutMap) before the press is delivered to
// the matcher's application filter. The keys come from the manifest of the
// built-in plugin `shell` through the real roostd and `keymap.get`; the window,
// the daemon and the fake `claude` are helpers/sessions_window.hpp. HOME,
// ROOST_SOCKET and ROOST_CONFIG are temporary; the bug report is opened, never
// created.

#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQmlExtensionPlugin>
#include <QSaveFile>
#include <QTest>

#include <optional>

#include "helpers/scoped_env.hpp"
#include "helpers/sessions_window.hpp"

using namespace roost;
using namespace roost::testing;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

// Counts the presses of a key that reach the window (not those of the
// modifiers that keyClick sends first), seen by an application filter that is
// installed after the matcher's and so is called before it.
class WindowPresses : public QObject {
public:
    WindowPresses(QObject* window, int key) : Window_(window), Key_(key) {
        qApp->installEventFilter(this);
    }
    ~WindowPresses() override { qApp->removeEventFilter(this); }

    int Count = 0;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == Window_ && event->type() == QEvent::KeyPress &&
            static_cast<QKeyEvent*>(event)->key() == Key_) {
            ++Count;
        }
        return false;
    }

private:
    QObject* Window_;
    int Key_;
};

const QString KEPT = QStringLiteral("kept");

}  // namespace

class ShellCommandsTest : public SessionsWindow {
    Q_OBJECT

private:
    // A user keymap.json, there before the daemon starts.
    bool WriteKeymap(const QByteArray& json) {
        if (!QDir().mkpath(Dir_->filePath("config"))) {
            return false;
        }
        QSaveFile file(Dir_->filePath("config/keymap.json"));
        return file.open(QIODevice::WriteOnly) && file.write(json) >= 0 &&
               file.commit();
    }

    QObject* BugReport() {
        return Root()->findChild<QObject*>("bugReportDialog");
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

    // The key is an entry of the table, not a Shortcut: the press reaches the
    // window once, the matcher takes it, and the window has no Shortcut that
    // Qt would try first.
    void CtrlBackslashMovesTheFocusToTheListThroughTheTable() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(!Root()->findChildren<QObject*>().isEmpty());
        for (const QObject* child : Root()->findChildren<QObject*>()) {
            QVERIFY2(!child->inherits("QQuickShortcut"),
                     qPrintable(child->objectName()));
        }
        QVERIFY(Api_->Commands().HasLocal("shell.focusList"));

        OpenRow(1);
        QVERIFY(Terminal()->hasActiveFocus());
        WindowPresses presses(Window(), Qt::Key_Backslash);
        Press(Qt::Key_Backslash, CONTROL_KEY);
        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);
        QCOMPARE(presses.Count, 1);
        // Not a toggle: in the list the key does nothing.
        Press(Qt::Key_Backslash, CONTROL_KEY);
        QTest::qWait(100);
        QVERIFY(ListView()->hasActiveFocus());
        QCOMPARE(presses.Count, 2);
    }

    // A `null` entry of the user's keymap.json for the command takes the key
    // away: roostd resolves it with the keys of the other plugins.
    void ANullEntryOfTheUserKeymapTakesCtrlBackslashAway() {
        FailOnWarnings();
        QVERIFY(WriteKeymap(
            R"([{"key": "Ctrl+\\", "mac": "Ctrl+\\", "command": null}])"));
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        QVERIFY(Terminal()->hasActiveFocus());
        Press(Qt::Key_Backslash, CONTROL_KEY);
        QTest::qWait(300);
        QVERIFY(Terminal()->hasActiveFocus());
        QVERIFY(!ListView()->hasActiveFocus());
    }

    // macOS only (Cmd+Shift+B); elsewhere the key is the terminal program's.
    // With the keyboard in a terminal: TerminalItem does not take the key.
    void CmdShiftBOpensTheBugReportOnMacOnly() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(0);
        QVERIFY(Terminal()->hasActiveFocus());
        QVERIFY(BugReport() != nullptr);
        QVERIFY(!BugReport()->property("visible").toBool());
        Press(Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier);
#ifdef Q_OS_MACOS
        QTRY_VERIFY_WITH_TIMEOUT(BugReport()->property("visible").toBool(),
                                 TIMEOUT_MS);
#else
        QTest::qWait(300);
        QVERIFY(!BugReport()->property("visible").toBool());
#endif
    }

    // terminal.copy is the terminal panel's: the command copies the selection
    // of the terminal, and goes when the panel does.
    void TheTerminalPanelHandlesTerminalCopy() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(0);
        QVERIFY(Api_->Commands().HasLocal("terminal.copy"));
        TerminalItem* const terminal = Terminal();
        // The first row of the screen has the greeting of the fake TUI.
        const QPoint row0 = terminal->mapToScene(QPointF(2, 4)).toPoint();
        QTest::mousePress(Window(), Qt::LeftButton, Qt::NoModifier, row0);
        QTest::mouseMove(Window(), row0 + QPoint(60, 0));
        QTest::mouseRelease(Window(), Qt::LeftButton, Qt::NoModifier,
                            row0 + QPoint(60, 0));
        QVERIFY(terminal->hasSelection());

        QGuiApplication::clipboard()->setText(KEPT);
        const QFuture<rpc::Result> done = Api_->Commands().Run("terminal.copy");
        QTRY_VERIFY_WITH_TIMEOUT(done.isFinished(), TIMEOUT_MS);
        QVERIFY(done.result());
        const QString copied = QGuiApplication::clipboard()->text();
        QVERIFY2(!copied.isEmpty() && copied != KEPT, qPrintable(copied));
    }
};

QTEST_MAIN(ShellCommandsTest)
#include "shell_commands_test.moc"
