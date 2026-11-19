// The built-in plugin `terminal`: its panel terminal.view in the shell's
// window (Main.qml of module Roost.Shell), against the real roostd with the
// real Claude plugin behind a fake `claude` whose `attach` runs fake_tui. The
// list is a fixture panel of a plugin in the test's own directory, whose
// output `selected` the test sets. What the user sees is read from the panel:
// the status of its TerminalItem, the screen of the session and the status
// text; keys go through the window, to whatever has the focus.

#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

#include <signal.h>

#include <algorithm>
#include <memory>

#include "api/api.hpp"
#include "core/rpc_client.hpp"
#include "helpers/window_teardown.hpp"
#include "qml_api/roost_singleton.hpp"
#include "qml_api/terminal_item.hpp"
#include "roost/plugin_loader.hpp"
#include "roost/shell.hpp"
#include "terminal/session_pane.hpp"
#include "terminal/terminal_session.hpp"

using namespace roost;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

constexpr int TIMEOUT_MS = 10000;
const QString LIST = "sessions.list";
const QString TERMINAL = "terminal.view";
const QString FIRST = "bg-1b2c3d4e";
const QString SECOND = "bg-0a1b2c3d";
const QString EMPTY_TEXT = "No session. Choose one in the list.";
// A session that the claude plugin does not list, so that its open is refused.
const QString UNKNOWN = "bg-deadbeef";

// Each query answered once; a digit after the 1 would be more than once.
const QRegularExpression ANSWERED_ONCE(
    QStringLiteral(R"(answers da1=1 cpr=1(?!\d))"));

// What row 5 of the TUI shows once "world" has arrived, whatever the split. No
// 'e', 'q' or 'x': fake_tui takes them for commands when one arrives in a read
// of its own.
const QRegularExpression TYPED_WORLD(
    QStringLiteral(R"(input (?:d|ld|rld|orld|world) total=5(?!\d))"));

// A fake `claude`, steered by the files next to it: agents.json is what
// `agents` prints, and `attach <id>` runs fake_tui, which shows the id as its
// greeting. In the root directory, so that the greeting shares its row with a
// short cwd and does not wrap.
constexpr const char* FAKE_CLAUDE = R"sh(#!/bin/sh
dir=$(dirname "$0")
case "$1" in
agents) cat "$dir/agents.json" ;;
attach) cd / && FAKE_TUI_GREETING=$2 exec "$dir/fake_tui" ;;
*) exit 2 ;;
esac
)sh";

// A list panel: its output is `selected`.
const QByteArray LIST_PANEL = R"(
import QtQuick
Item {
    property var selected: null
}
)";

// Whether a roostd, perhaps still starting, answers hello at the path.
bool Greets(const QString& socketPath) {
    const QDeadlineTimer deadline(TIMEOUT_MS);
    while (!deadline.hasExpired()) {
        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(socketPath);
        if (QTest::qWaitFor([&] { return hello.isFinished(); }, TIMEOUT_MS) &&
            hello.result()) {
            return true;
        }
        QTest::qWait(20);
    }
    return false;
}

// What the terminal shows, as the VT sequences that rebuild it.
QString Screen(const std::shared_ptr<TerminalSession>& session) {
    if (!session) {
        return {};
    }
    const auto lock = session->Lock();
    return QString::fromUtf8(session->Dump(lock).value_or("dump failed"));
}

// Every warning fails the test but one of Qt's font database, which the first
// text of a process draws with and which depends on the machine's fonts.
void FailOnWarnings() {
    QTest::failOnWarning(QRegularExpression(
        QStringLiteral(R"(^(?!Populating font family aliases))"),
        QRegularExpression::DotMatchesEverythingOption));
}

QString Greeting(const QString& id) { return "greeting=" + id; }

QVariantMap Ref(const QString& id) { return {{"agent", "claude"}, {"id", id}}; }

QByteArray ReadFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

}  // namespace

class TerminalPanelTest : public QObject {
    Q_OBJECT

private:
    QString SocketPath() const { return Dir_->filePath("roostd.sock"); }
    QString OwnPluginsPath() const { return Dir_->filePath("plugins"); }
    QString BuiltInPluginsPath() const { return Dir_->filePath("built-in"); }

    bool WriteFile(const QString& path, const QByteArray& content) const {
        QFile file(path);
        return QDir().mkpath(QFileInfo(path).absolutePath()) &&
               file.open(QIODevice::WriteOnly) &&
               file.write(content) == content.size();
    }

    bool WriteFakeClaude() const {
        const QDir bin(Dir_->filePath("bin"));
        QFile script(bin.filePath("claude"));
        if (!bin.mkpath(".") || !script.open(QIODevice::WriteOnly) ||
            script.write(FAKE_CLAUDE) < 0 ||
            !script.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                                   QFile::ExeOwner)) {
            return false;
        }
        script.close();
        return QFile::copy(QStringLiteral(FIXTURES_DIR "/claude_agents.json"),
                           bin.filePath("agents.json")) &&
               QFile::link(FAKE_TUI_PATH, bin.filePath("fake_tui"));
    }

    // The fixture list: a plugin with the panel sessions.list, which has the
    // one output `selected` of type SessionRef, and no backend.
    bool WriteListPlugin() const {
        const QJsonObject panel{
            {"id", "list"},
            {"qml", "list.qml"},
            {"inputs", QJsonObject{}},
            {"outputs", QJsonObject{{"selected", "SessionRef"}}}};
        const QJsonObject manifest{
            {"id", "sessions"},
            {"version", "0.1.0"},
            {"api", "0"},
            {"contributes", QJsonObject{{"panels", QJsonArray{panel}}}}};
        const QString directory = OwnPluginsPath() + "/sessions";
        return WriteFile(directory + "/plugin.json",
                         QJsonDocument(manifest).toJson()) &&
               WriteFile(directory + "/list.qml", LIST_PANEL);
    }

    // Only the plugins the test needs of the built-in ones: the built-in
    // `sessions` has the panel sessions.list that the fixture list has.
    bool LinkBuiltInPlugins() const {
        if (!QDir().mkpath(BuiltInPluginsPath())) {
            return false;
        }
        for (const QString& id :
             {QStringLiteral("claude"), QStringLiteral("terminal")}) {
            if (!QFile::link(QStringLiteral(ROOST_PLUGINS_DIR "/") + id,
                             BuiltInPluginsPath() + "/" + id)) {
                return false;
            }
        }
        return true;
    }

    bool LaunchDaemon() {
        if (!WriteFakeClaude() || !WriteListPlugin() || !LinkBuiltInPlugins()) {
            return false;
        }
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("ROOST_SOCKET", SocketPath());
        environment.insert("ROOST_CONFIG", Dir_->filePath("config"));
        environment.insert(
            "ROOST_PLUGINS_PATH",
            BuiltInPluginsPath() + QDir::listSeparator() + OwnPluginsPath());
        environment.insert(
            "PATH", Dir_->filePath("bin") + u':' + environment.value("PATH"));
        Daemon_ = std::make_unique<QProcess>();
        Daemon_->setProcessEnvironment(environment);
        Daemon_->setProcessChannelMode(QProcess::ForwardedErrorChannel);
        Daemon_->start(ROOSTD_PATH, {});
        return Daemon_->waitForStarted(TIMEOUT_MS) && Greets(SocketPath());
    }

    // The window of the shell, connected to the daemon, with the panels
    // loaded; waits until both slots hold theirs.
    void Boot() {
        QVERIFY(LaunchDaemon());
        Api_ = std::make_unique<api::Api>();
        Engine_ = std::make_unique<QQmlApplicationEngine>();
        Singleton_ =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        QVERIFY(Singleton_ != nullptr);
        Singleton_->setApi(*Api_);
        Api_->Connect(SocketPath());
        Engine_->loadFromModule("Roost.Shell", "Main");
        QVERIFY(!Engine_->rootObjects().isEmpty());
        Loader_ = std::make_unique<PluginLoader>(*Engine_, Api_->Plugins(),
                                                 *Singleton_);
        Shell_ = std::make_unique<Shell>(*Engine_->rootObjects().first(),
                                         *Loader_, *Api_, SocketPath());
        QTRY_VERIFY_WITH_TIMEOUT(
            PanelOf(LIST) != nullptr && PanelOf(TERMINAL) != nullptr,
            TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Root()->property("connection").toString(),
                                  QString("connected"), TIMEOUT_MS);
        auto* window = qobject_cast<QQuickWindow*>(Root());
        QVERIFY(window != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QVERIFY(QTest::qWaitForWindowActive(window));
    }

    QObject* Root() { return Engine_->rootObjects().first(); }
    QQuickWindow* Window() { return qobject_cast<QQuickWindow*>(Root()); }

    QQuickItem* Slot(const QString& type) {
        for (auto* item : Root()->findChildren<QQuickItem*>("panelSlot")) {
            if (item->property("panelType").toString() == type) {
                return item;
            }
        }
        return nullptr;
    }

    QQuickItem* PanelOf(const QString& type) {
        QQuickItem* slot = Slot(type);
        return slot == nullptr ? nullptr
                               : slot->property("panel").value<QQuickItem*>();
    }

    TerminalItem* Terminal() {
        return PanelOf(TERMINAL)->findChild<TerminalItem*>("terminal");
    }

    QQuickItem* StatusText() {
        return PanelOf(TERMINAL)->findChild<QQuickItem*>("terminalStatus");
    }

    QString Status() { return Terminal()->status(); }

    QString StatusTextNow() {
        return StatusText()->property("text").toString();
    }

    // Records "<status>|<text>" at each change of the terminal's status, from
    // now on, so that a state that passes quickly is still seen.
    void Watch() {
        Changes_.clear();
        connect(Terminal(), &TerminalItem::statusChanged, this,
                [this] { Changes_ << Status() + "|" + StatusTextNow(); });
    }

    // What the session in the terminal shows now; empty without one.
    QString ScreenNow() {
        const auto* pane = Terminal()->findChild<SessionPane*>();
        return pane == nullptr ? QString{} : Screen(pane->Session());
    }

    // The list's selection, as the user's click on a row would write it.
    void Select(const QVariant& selected) {
        QVERIFY(PanelOf(LIST)->setProperty("selected", selected));
    }

    // The terminal's slot takes the focus, as it does when the user moves
    // there by keys.
    void FocusTerminalSlot() {
        Slot(TERMINAL)->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(Terminal()->hasActiveFocus(), TIMEOUT_MS);
    }

    // Waits until the session has had its queries answered: fake_tui reacts
    // to a key only if the key arrives in a read of its own.
    void Settle() {
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(ANSWERED_ONCE),
                                 TIMEOUT_MS);
    }

    // A session open, with its queries answered, in the focused terminal, that
    // then ends by itself.
    void OpenAndEnd() {
        Select(Ref(FIRST));
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        Settle();
        FocusTerminalSlot();
        QTest::keyClick(Window(), 'x');
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("ended"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(StatusText()->isVisible(), TIMEOUT_MS);
    }

    QStringList Problems() {
        return Root()->property("problems").toStringList();
    }

    QStringList Changes_;
    std::unique_ptr<QTemporaryDir> Dir_;
    std::unique_ptr<QProcess> Daemon_;
    std::unique_ptr<api::Api> Api_;
    std::unique_ptr<QQmlApplicationEngine> Engine_;
    RoostSingleton* Singleton_ = nullptr;
    std::unique_ptr<PluginLoader> Loader_;
    std::unique_ptr<Shell> Shell_;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
    }

    void cleanup() {
        // The shell before the loader, the loader before the engine, which
        // holds the terminal that closes its session through the API.
        Shell_.reset();
        Loader_.reset();
        // The window stops rendering before it is destroyed.
        roost::testing::QuiesceWindows(Engine_ ? Engine_->rootObjects()
                                               : QObjectList{});
        Engine_.reset();
        Singleton_ = nullptr;
        Api_.reset();
        if (Daemon_ && Daemon_->state() != QProcess::NotRunning) {
            QCOMPARE(::kill(static_cast<pid_t>(Daemon_->processId()), SIGTERM),
                     0);
            QVERIFY(Daemon_->waitForFinished(TIMEOUT_MS));
            QCOMPARE(Daemon_->exitStatus(), QProcess::NormalExit);
            QCOMPARE(Daemon_->exitCode(), 0);
        }
        Daemon_.reset();
    }

    void TheTerminalSlotHoldsThePanelOfThePluginLinkedToTheSession() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        QQuickItem* panel = PanelOf(TERMINAL);
        QCOMPARE(panel->parentItem(), Slot(TERMINAL));
        QCOMPARE(Slot(TERMINAL)->property("linkInput").toString(),
                 QString("session"));
        QCOMPARE(Slot(LIST)->property("linkOutput").toString(),
                 QString("selected"));
        QVERIFY(Terminal() != nullptr);
        QVERIFY(Problems().isEmpty());
        // Nothing chosen yet.
        QCOMPARE(Status(), QString("none"));
        QVERIFY(StatusText()->isVisible());
        QCOMPARE(StatusText()->property("text").toString(), EMPTY_TEXT);
    }

    void TheSessionChosenInTheListIsOpenedAndShown() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        Select(Ref(FIRST));

        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(FIRST)),
                                 TIMEOUT_MS);
        QVERIFY(!StatusText()->isVisible());
        QVERIFY(StatusText()->property("text").toString().isEmpty());
    }

    void NoSelectionShowsTheEmptyText() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Select(Ref(FIRST));
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);

        Select(QVariant::fromValue(nullptr));

        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("none"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(StatusText()->isVisible(), TIMEOUT_MS);
        QCOMPARE(StatusText()->property("text").toString(), EMPTY_TEXT);
    }

    void AnotherSelectionSwitchesTheTerminalToThatSession() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Select(Ref(FIRST));
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(FIRST)),
                                 TIMEOUT_MS);

        Select(Ref(SECOND));

        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(SECOND)),
                                 TIMEOUT_MS);
        QCOMPARE(Status(), QString("open"));
        QVERIFY(!ScreenNow().contains(Greeting(FIRST)));
    }

    void TheKeysOfTheKeyboardReachTheTuiOfTheFocusedTerminalSlot() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Select(Ref(FIRST));
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        Settle();

        FocusTerminalSlot();
        for (const char key : {'w', 'o', 'r', 'l', 'd'}) {
            QTest::keyClick(Window(), key);
            // Each key in a read of its own: the split a slow runner makes.
            QTest::qWait(50);
        }

        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(TYPED_WORLD), TIMEOUT_MS);
    }

    void AnEndedSessionIsSaidSoWithTheReasonAndEnterReopensIt() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        OpenAndEnd();
        if (QTest::currentTestFailed()) {
            return;
        }

        const QString reason = Terminal()->statusMessage();
        QVERIFY(!reason.isEmpty());
        QCOMPARE(StatusTextNow(),
                 "Session ended: " + reason + ". Press Enter to reopen.");

        QTest::keyClick(Window(), Qt::Key_Return);

        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(FIRST)),
                                 TIMEOUT_MS);
        QVERIFY(StatusTextNow().isEmpty());
    }

    void EnterOfTheKeypadReopensAnEndedSession() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenAndEnd();
        if (QTest::currentTestFailed()) {
            return;
        }

        QTest::keyClick(Window(), Qt::Key_Enter, Qt::KeypadModifier);

        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(FIRST)),
                                 TIMEOUT_MS);
    }

    void EnterInALiveSessionGoesToItsProgram() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Select(Ref(FIRST));
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        Settle();
        FocusTerminalSlot();

        QTest::keyClick(Window(), Qt::Key_Return);

        // fake_tui shows the one byte it got: carriage return.
        QTRY_VERIFY_WITH_TIMEOUT(
            ScreenNow().contains(QStringLiteral("input \\x0d total=1")),
            TIMEOUT_MS);
        QCOMPARE(Status(), QString("open"));
    }

    void OpeningIsSaidWithTheNameOfTheSession() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Watch();

        Select(Ref(FIRST));

        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QVERIFY2(
            Changes_.contains("opening|Opening claude/" + FIRST + "\u2026"),
            qPrintable(Changes_.join(", ")));
    }

    void ASessionThatCannotBeOpenedIsSaidSoAndEnterTriesAgain() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        Select(Ref(UNKNOWN));

        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("failed"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(StatusText()->isVisible(), TIMEOUT_MS);
        const QString reason = Terminal()->statusMessage();
        QVERIFY(!reason.isEmpty());
        QCOMPARE(StatusTextNow(), "Could not open the session: " + reason +
                                      ". Press Enter to try again.");

        FocusTerminalSlot();
        Watch();
        QTest::keyClick(Window(), Qt::Key_Return);

        // It went through opening again, and failed again.
        QTRY_VERIFY_WITH_TIMEOUT(
            std::ranges::any_of(Changes_,
                                [](const QString& change) {
                                    return change.startsWith("opening|");
                                }),
            TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("failed"), TIMEOUT_MS);
    }

    void AChosenSessionNeverReadsNoSessionBeforeTheItemHasASize() {
        FailOnWarnings();
        // An API that is not connected: the item gets as far as ensuring its
        // pane, and waits for a size.
        api::Api api;
        QQmlEngine engine;
        auto* singleton =
            engine.singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        QVERIFY(singleton != nullptr);
        singleton->setApi(api);
        QQmlComponent component(
            &engine, QUrl::fromLocalFile(QStringLiteral(
                         TERMINAL_PLUGIN_DIR "/qml/TerminalPanel.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        // No window and no size: the item has not started opening.
        const std::unique_ptr<QObject> panel(component.create());
        QVERIFY(panel != nullptr);
        auto* terminal = panel->findChild<TerminalItem*>("terminal");
        auto* text = panel->findChild<QQuickItem*>("terminalStatus");
        QVERIFY(terminal != nullptr && text != nullptr);

        QCOMPARE(text->property("text").toString(), EMPTY_TEXT);
        QVERIFY(panel->setProperty("session", Ref(FIRST)));

        QCOMPARE(terminal->status(), QString("none"));
        QCOMPARE(text->property("text").toString(), QString());
        QVERIFY(!text->isVisible());
    }

    // The build holds copies of the plugin's files (configure_file); a copy
    // that is older than the source is a panel the window does not show.
    void TheBuildCopiesOfThePluginAreTheSourceFiles() {
        for (const QString& file : {QStringLiteral("plugin.json"),
                                    QStringLiteral("qml/TerminalPanel.qml")}) {
            const QByteArray source =
                ReadFile(QStringLiteral(TERMINAL_PLUGIN_DIR "/") + file);
            const QByteArray copy =
                ReadFile(QStringLiteral(ROOST_PLUGINS_DIR "/terminal/") + file);
            QVERIFY2(!source.isEmpty(), qPrintable(file));
            QVERIFY2(source == copy,
                     qPrintable(file + ": the build copy is stale"));
        }
    }
};

QTEST_MAIN(TerminalPanelTest)
#include "terminal_panel_test.moc"
