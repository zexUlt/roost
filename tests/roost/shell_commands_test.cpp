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
#include <QQmlContext>
#include <QQmlExtensionPlugin>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTest>

#include <memory>
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

// The key of terminal.copy as the manifest of `shell` has it: Ctrl+Shift+C,
// Cmd+C on macOS (Qt's Control is Command there).
#ifdef Q_OS_MACOS
constexpr Qt::KeyboardModifiers COPY_MODIFIERS = Qt::ControlModifier;
#else
constexpr Qt::KeyboardModifiers COPY_MODIFIERS =
    Qt::ControlModifier | Qt::ShiftModifier;
#endif

// A handler of a command that counts its runs and does nothing else.
struct Counter {
    QObject Owner;
    int Runs = 0;
};

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

    // The handler of `command` that counts, the most recent registration.
    void Count(const QString& command, Counter& counter) {
        QVERIFY(Api_->Commands().Register(
            command, &counter.Owner, [&counter](const QJsonValue&) {
                ++counter.Runs;
                return rpc::Result(QJsonValue::Null);
            }));
    }

    // The shortcuts of the window, with a spy on the activations of each.
    QList<QObject*> Shortcuts() {
        QList<QObject*> found;
        for (QObject* child : Root()->findChildren<QObject*>()) {
            if (child->inherits("QQuickShortcut")) {
                found.append(child);
            }
        }
        return found;
    }

    // A selection in the first row of the open terminal, made with the mouse.
    void SelectInTerminal() {
        const QPoint row0 = Terminal()->mapToScene(QPointF(2, 4)).toPoint();
        QTest::mousePress(Window(), Qt::LeftButton, Qt::NoModifier, row0);
        QTest::mouseMove(Window(), row0 + QPoint(60, 0));
        QTest::mouseRelease(Window(), Qt::LeftButton, Qt::NoModifier,
                            row0 + QPoint(60, 0));
        QVERIFY(Terminal()->hasSelection());
    }

    // The clipboard has something that is not KEPT.
    static bool Copied() {
        const QString text = QGuiApplication::clipboard()->text();
        return !text.isEmpty() && text != KEPT;
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
        QVERIFY(Api_->Commands().HasLocal("shell.focusList"));
        // The window's two Shortcuts are off while the table is there.
        const QList<QObject*> shortcuts = Shortcuts();
        QCOMPARE(shortcuts.size(), 2);
        for (QObject* shortcut : shortcuts) {
            QVERIFY(!shortcut->property("enabled").toBool());
        }

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

    // Counts the activations of both paths of Ctrl+\ on the real key path: with
    // the table loaded the command runs once and the Shortcuts stay silent;
    // with no table (roostd gone) a Shortcut moves the focus and the command
    // is not run.
    void CtrlBackslashRunsOnceWhateverTheTableState() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Counter counter;
        Count("shell.focusList", counter);
        QList<std::shared_ptr<QSignalSpy>> spies;
        for (QObject* shortcut : Shortcuts()) {
            spies.append(
                std::make_shared<QSignalSpy>(shortcut, SIGNAL(activated())));
        }
        QCOMPARE(spies.size(), 2);
        const auto activations = [&spies] {
            int total = 0;
            for (const auto& spy : spies) {
                total += static_cast<int>(spy->size());
            }
            return total;
        };

        // The table is loaded.
        OpenRow(0);
        QVERIFY(Terminal()->hasActiveFocus());
        Press(Qt::Key_Backslash, CONTROL_KEY);
        QTRY_COMPARE_WITH_TIMEOUT(counter.Runs, 1, TIMEOUT_MS);
        QTest::qWait(200);
        QCOMPARE(counter.Runs, 1);
        QCOMPARE(activations(), 0);
        QVERIFY(Terminal()->hasActiveFocus());

        // The table is gone with the daemon.
        StopDaemon();
        QTRY_VERIFY_WITH_TIMEOUT(!Api_->Keymap().IsLoaded(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!Singleton_->keymapLoaded(), TIMEOUT_MS);
        // Both Shortcuts are on now (Cmd+Shift+B on macOS only).
        for (QObject* shortcut : Shortcuts()) {
            const bool shifted =
                shortcut->property("sequence").toString().contains("Shift");
#ifdef Q_OS_MACOS
            constexpr bool bugReportShortcutOn = true;
#else
            constexpr bool bugReportShortcutOn = false;
#endif
            QTRY_COMPARE_WITH_TIMEOUT(shortcut->property("enabled").toBool(),
                                      !shifted || bugReportShortcutOn,
                                      TIMEOUT_MS);
        }
        QVERIFY(Terminal()->hasActiveFocus());
        Press(Qt::Key_Backslash, CONTROL_KEY);
        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);
        QCOMPARE(activations(), 1);
        QCOMPARE(counter.Runs, 1);
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
        SelectInTerminal();
        QVERIFY(terminal->hasSelection());

        QGuiApplication::clipboard()->setText(KEPT);
        const QFuture<rpc::Result> done = Api_->Commands().Run("terminal.copy");
        QTRY_VERIFY_WITH_TIMEOUT(done.isFinished(), TIMEOUT_MS);
        QVERIFY(done.result());
        const QString copied = QGuiApplication::clipboard()->text();
        QVERIFY2(!copied.isEmpty() && copied != KEPT, qPrintable(copied));
    }

    // The key of terminal.copy copies the selection of the focused terminal.
    void TheCopyKeyCopiesTheSelectionOfTheFocusedTerminal() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(0);
        SelectInTerminal();
        QGuiApplication::clipboard()->setText(KEPT);
        Press(Qt::Key_C, COPY_MODIFIERS);
        QTRY_VERIFY_WITH_TIMEOUT(Copied(), TIMEOUT_MS);
    }

    // The key is the table's: the command runs, and the item never sees the
    // key (a copy by IsCopyShortcut would change the clipboard).
    void TheCopyKeyRunsTheCommandOfTheTableInTheTerminal() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(0);
        SelectInTerminal();
        Counter counter;
        Count("terminal.copy", counter);
        QGuiApplication::clipboard()->setText(KEPT);
        Press(Qt::Key_C, COPY_MODIFIERS);
        QTRY_COMPARE_WITH_TIMEOUT(counter.Runs, 1, TIMEOUT_MS);
        QTest::qWait(200);
        QCOMPARE(QGuiApplication::clipboard()->text(), KEPT);
        QCOMPARE(counter.Runs, 1);
    }

    // The entry has `when` terminal.view: in the list the key is not run.
    void TheCopyKeyDoesNotFireOutsideTheTerminalView() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(0);
        SelectInTerminal();
        BackToList();
        Counter counter;
        Count("terminal.copy", counter);
        QGuiApplication::clipboard()->setText(KEPT);
        Press(Qt::Key_C, COPY_MODIFIERS);
        QTest::qWait(300);
        QCOMPARE(counter.Runs, 0);
        QCOMPARE(QGuiApplication::clipboard()->text(), KEPT);
    }

    // Without a handler in the window the table's entry does not take the
    // key: it is not sent to roostd for -32003 (a warning would fail the
    // case), and the terminal copies by IsCopyShortcut.
    void WithNoLocalHandlerTheCopyKeyFallsThroughToTheTerminal() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(0);
        SelectInTerminal();
        // The owner of the panel's calls goes, as in the first moments of an
        // unload: the panel is still the one with the focus.
        // (a plain QObject child of the panel's context, above the contexts
        // of its files)
        for (QQmlContext* context = qmlContext(PanelOf(TERMINAL));
             context != nullptr; context = context->parentContext()) {
            for (QObject* child : context->children()) {
                if (child->metaObject() == &QObject::staticMetaObject) {
                    delete child;
                }
            }
        }
        QVERIFY(!Api_->Commands().HasLocal("terminal.copy"));
        QVERIFY(Terminal()->hasActiveFocus());
        QGuiApplication::clipboard()->setText(KEPT);
        Press(Qt::Key_C, COPY_MODIFIERS);
        QTRY_VERIFY_WITH_TIMEOUT(Copied(), TIMEOUT_MS);
    }

    // A `null` for terminal.copy in keymap.json takes the table's key away;
    // the terminal still copies by IsCopyShortcut, as the design says.
    void ANullForTerminalCopyStillCopiesByTheFallback() {
        FailOnWarnings();
        QVERIFY(WriteKeymap(
            R"([{"key": "Ctrl+Shift+C", "mac": "Cmd+C", "when": "terminal.view", "command": null}])"));
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(0);
        SelectInTerminal();
        Counter counter;
        Count("terminal.copy", counter);
        QGuiApplication::clipboard()->setText(KEPT);
        Press(Qt::Key_C, COPY_MODIFIERS);
        QTRY_VERIFY_WITH_TIMEOUT(Copied(), TIMEOUT_MS);
        QCOMPARE(counter.Runs, 0);
    }

    // A panel created before the old one is destroyed (a reload), and the
    // first of two panels closed: the command stays and still copies.
    void TheCopyCommandSurvivesTwoPanelsAndTheOrderOfTheirEnd() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(0);
        QQuickItem* const first = PanelOf(TERMINAL);
        QQuickItem* const second = Loader_->CreatePanel(TERMINAL);
        QVERIFY(second != nullptr);
        QVERIFY(Api_->Commands().HasLocal("terminal.copy"));
        Loader_->DestroyPanel(first);
        QVERIFY(Api_->Commands().HasLocal("terminal.copy"));
        const QFuture<rpc::Result> done = Api_->Commands().Run("terminal.copy");
        QTRY_VERIFY_WITH_TIMEOUT(done.isFinished(), TIMEOUT_MS);
        QVERIFY(done.result());
        Loader_->DestroyPanel(second);
        QVERIFY(!Api_->Commands().HasLocal("terminal.copy"));
    }
};

QTEST_MAIN(ShellCommandsTest)
#include "shell_commands_test.moc"
