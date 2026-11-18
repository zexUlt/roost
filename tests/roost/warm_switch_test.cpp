// Warm terminals at the level of the window (design.md §3): the shell's
// window with the real terminal panel, against the real roostd with the real
// Claude plugin behind a fake `claude` whose `attach` runs fake_tui, as in
// terminal_panel_test.
//
// A warm switch is one frame: the first frame the scene graph syncs after the
// selection already shows the new session, whole, without session.open and
// without `opening`. The frame is counted, not timed: a direct connection to
// afterSynchronizing sees the frame in the scene graph's own thread, where the
// GUI thread is blocked and nothing else changes the item or its terminal, so
// what it reads is that frame and no other, under the basic and the threaded
// render loop alike. ctest runs the test under both (QSG_RENDER_LOOP), and the
// test checks that the sync ran in the thread that the loop promises.

#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <signal.h>

#include <atomic>
#include <memory>

#include "api/api.hpp"
#include "core/rpc_client.hpp"
#include "helpers/counting_proxy.hpp"
#include "qml_api/roost_singleton.hpp"
#include "qml_api/terminal_item.hpp"
#include "roost/plugin_loader.hpp"
#include "roost/shell.hpp"
#include "terminal/io_thread.hpp"
#include "terminal/session_pane.hpp"
#include "terminal/terminal_session.hpp"
#include "terminal/warm_terminals.hpp"

using namespace roost;
using roost::testing::CountingProxy;

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
const QString HIDDEN_TEXT = "warm-B-1";

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
// greeting, after it has put the id on a line of attach.log. In the root
// directory, so that the greeting shares its row with a short cwd and does not
// wrap.
constexpr const char* FAKE_CLAUDE = R"sh(#!/bin/sh
dir=$(dirname "$0")
case "$1" in
agents) cat "$dir/agents.json" ;;
attach) echo "$2" >> "$dir/attach.log"
        cd / && FAKE_TUI_GREETING=$2 exec "$dir/fake_tui" ;;
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

// The rows of the last frame the session was drawn in, one text per row. Only
// the thread that draws changes them (BeginRenderUpdate), so the scene graph's
// thread reads them without the lock, as terminal_item_mouse_test does.
QString RenderedRows(const TerminalSession& session) {
    QStringList rows;
    for (const auto& row : session.RenderState().Rows()) {
        QString text;
        for (const auto& cell : row.Cells) {
            text += cell.Text.isEmpty() ? QStringLiteral(" ") : cell.Text;
        }
        rows << text;
    }
    return rows.join(u'\n');
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

SessionKey Key(const QString& id) { return {"claude", id}; }

// What the first frame of the scene graph after the arming drew: read in the
// scene graph's thread, in the sync, where the GUI thread is blocked.
class FirstSync {
public:
    // Connected before the change that the frame must show, with no event
    // processed in between: no frame can be synced by then.
    FirstSync(QQuickWindow& window, const TerminalItem& item,
              std::shared_ptr<TerminalSession> shown)
        : GuiThread_(QThread::currentThread()),
          Connection_(QObject::connect(
              &window, &QQuickWindow::afterSynchronizing, &window,
              [this, &item, shown = std::move(shown)] {
                  if (Done_.load()) {
                      return;
                  }
                  OffGuiThread_ = QThread::currentThread() != GuiThread_;
                  RowsRebuilt_ = item.rowsRebuilt();
                  Rows_ = item.rows();
                  Text_ = RenderedRows(*shown);
                  Done_.store(true);
              },
              Qt::DirectConnection)) {}

    ~FirstSync() { QObject::disconnect(Connection_); }

    FirstSync(const FirstSync&) = delete;
    FirstSync& operator=(const FirstSync&) = delete;

    bool Done() const { return Done_.load(); }
    // The rest only once Done.
    int RowsRebuilt() const { return RowsRebuilt_; }
    int Rows() const { return Rows_; }
    const QString& Text() const { return Text_; }
    // Whether the sync ran in a thread other than the GUI thread.
    bool OffGuiThread() const { return OffGuiThread_; }

private:
    const QThread* GuiThread_;
    QMetaObject::Connection Connection_;
    std::atomic<bool> Done_ = false;
    bool OffGuiThread_ = false;
    int RowsRebuilt_ = -1;
    int Rows_ = -1;
    QString Text_;
};

}  // namespace

class WarmSwitchTest : public QObject {
    Q_OBJECT

private:
    QString SocketPath() const { return Dir_->filePath("roostd.sock"); }
    QString OwnPluginsPath() const { return Dir_->filePath("plugins"); }
    QString BuiltInPluginsPath() const { return Dir_->filePath("built-in"); }
    QString AttachLogPath() const { return Dir_->filePath("bin/attach.log"); }

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

    // The daemon keeps running when a window is destroyed.
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

    // A window of the shell, connected to the daemon through a proxy that
    // counts what the window sends, with the panels loaded; waits until both
    // slots hold theirs.
    void OpenWindow() {
        Proxy_ = std::make_unique<CountingProxy>(
            Dir_->filePath(QStringLiteral("proxy-%1.sock").arg(++Windows_)),
            SocketPath());
        QVERIFY(Proxy_->Listen());
        Api_ = std::make_unique<api::Api>();
        Engine_ = std::make_unique<QQmlApplicationEngine>();
        Singleton_ =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        QVERIFY(Singleton_ != nullptr);
        Singleton_->setApi(*Api_);
        Api_->Connect(Proxy_->Path());
        Engine_->loadFromModule("Roost.Shell", "Main");
        QVERIFY(!Engine_->rootObjects().isEmpty());
        Loader_ = std::make_unique<PluginLoader>(*Engine_, Api_->Plugins(),
                                                 *Singleton_);
        Shell_ = std::make_unique<Shell>(*Engine_->rootObjects().first(),
                                         *Loader_, *Api_, Proxy_->Path());
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

    // The window goes away as one that crashed does: its connection breaks
    // while its channels are open, and nothing is closed on the way out.
    void LoseWindow() {
        Proxy_.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!Api_->IsConnected(), TIMEOUT_MS);
        DestroyWindow();
    }

    void DestroyWindow() {
        // The shell before the loader, the loader before the engine, which
        // holds the terminal that closes its session through the API.
        Shell_.reset();
        Loader_.reset();
        Engine_.reset();
        Singleton_ = nullptr;
        Api_.reset();
        Proxy_.reset();
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

    // The screen of a session the window holds, shown or not.
    QString ScreenOf(const QString& id) {
        return Screen(Api_->Terminals().Session(Key(id)));
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

    // Shows the session in the terminal and waits until it is open, with the
    // greeting on the screen and its queries answered: fake_tui reacts to a
    // key only if the key arrives in a read of its own. Then waits for
    // a frame of it.
    void ShowAndSettle(const QString& id) {
        Select(Ref(id));
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(id)),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(ANSWERED_ONCE),
                                 TIMEOUT_MS);
        Draw(id);
    }

    // Waits until the scene graph has drawn what the shown session has now,
    // so that its next frame redraws only what changes afterwards. Without it
    // the basic render loop may not have drawn a session at all before it is
    // hidden, and its first frame would rebuild every row in any case.
    void Draw(const QString& id) {
        FirstSync sync(*Window(), *Terminal(),
                       Api_->Terminals().Session(Key(id)));
        Terminal()->update();
        QTRY_VERIFY_WITH_TIMEOUT(sync.Done(), TIMEOUT_MS);
    }

    // A round trip through the window's own connection: what the window sent
    // before has passed the proxy, and what roostd sent before the answer has
    // been delivered.
    bool RoundTrip() {
        QFuture<rpc::Result> round =
            Api_->Io().Request(QStringLiteral("plugins.list"));
        if (!QTest::qWaitFor([&] { return round.isFinished(); }, TIMEOUT_MS)) {
            return false;
        }
        QCoreApplication::processEvents();
        return round.result().has_value();
    }

    // How many times the fake claude has attached.
    int Attaches() const {
        QFile log(AttachLogPath());
        if (!log.open(QIODevice::ReadOnly)) {
            return 0;
        }
        return static_cast<int>(log.readAll().count('\n'));
    }

    // The program of the session gets input from a connection of its own,
    // which the window knows nothing of, and so acknowledges nothing for.
    bool TypeThroughAnotherChannel(const QString& id, const QByteArray& bytes) {
        Direct_ = std::make_unique<IoThread>();
        QFuture<rpc::Result> hello = Direct_->Connect(SocketPath());
        if (!QTest::qWaitFor([&] { return hello.isFinished(); }, TIMEOUT_MS) ||
            !hello.result()) {
            return false;
        }
        QFuture<OpenResult> open = Direct_->Open(
            "claude", id, static_cast<quint16>(Terminal()->columns()),
            static_cast<quint16>(Terminal()->rows()));
        if (!QTest::qWaitFor([&] { return open.isFinished(); }, TIMEOUT_MS) ||
            !open.result()) {
            return false;
        }
        Direct_->SendInput(open.result()->Channel, bytes);
        return true;
    }

    QStringList Changes_;
    int Windows_ = 0;
    std::unique_ptr<QTemporaryDir> Dir_;
    std::unique_ptr<QProcess> Daemon_;
    std::unique_ptr<CountingProxy> Proxy_;
    std::unique_ptr<api::Api> Api_;
    std::unique_ptr<QQmlApplicationEngine> Engine_;
    RoostSingleton* Singleton_ = nullptr;
    std::unique_ptr<PluginLoader> Loader_;
    std::unique_ptr<Shell> Shell_;
    std::unique_ptr<IoThread> Direct_;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
    }

    void cleanup() {
        if (Direct_) {
            Direct_->Stop();
            Direct_.reset();
        }
        DestroyWindow();
        if (Daemon_ && Daemon_->state() != QProcess::NotRunning) {
            QCOMPARE(::kill(static_cast<pid_t>(Daemon_->processId()), SIGTERM),
                     0);
            QVERIFY(Daemon_->waitForFinished(TIMEOUT_MS));
            QCOMPARE(Daemon_->exitStatus(), QProcess::NormalExit);
            QCOMPARE(Daemon_->exitCode(), 0);
        }
        Daemon_.reset();
    }

    // The first frame after the selection shows the other session, whole,
    // with what it printed while hidden; and the window sent no session.open
    // and said no `opening`.
    void AWarmSwitchIsOneFrame() {
        FailOnWarnings();
        QVERIFY(LaunchDaemon());
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        // Both sessions are warm, A is shown.
        ShowAndSettle(FIRST);
        ShowAndSettle(SECOND);
        ShowAndSettle(FIRST);
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(Api_->Terminals().Size(), std::size_t{2});
        QVERIFY(!Api_->Terminals().IsShown(Key(SECOND)));
        const auto hidden = Api_->Terminals().Session(Key(SECOND));
        QVERIFY(hidden != nullptr);

        // B prints while it is hidden: the window does not acknowledge it.
        QVERIFY(TypeThroughAnotherChannel(SECOND, HIDDEN_TEXT.toUtf8()));
        QTRY_VERIFY_WITH_TIMEOUT(
            ScreenOf(SECOND).contains("input " + HIDDEN_TEXT + " total=8"),
            TIMEOUT_MS);
        QVERIFY(RoundTrip());

        // Arm.
        Watch();
        const int opens = Proxy_->OpenRequests();
        const int attaches = Attaches();
        FirstSync sync(*Window(), *Terminal(), hidden);
        Select(Ref(SECOND));
        QTRY_VERIFY_WITH_TIMEOUT(sync.Done(), TIMEOUT_MS);

        // In the thread that the render loop promises.
        const QString loop = qEnvironmentVariable("QSG_RENDER_LOOP");
        if (loop == "threaded") {
            QVERIFY2(sync.OffGuiThread(), "threaded: the sync ran in the GUI");
        } else if (loop == "basic") {
            QVERIFY2(!sync.OffGuiThread(), "basic: the sync ran off the GUI");
        }

        // What that frame drew: every row of the grid, from B's terminal.
        QCOMPARE(sync.RowsRebuilt(), sync.Rows());
        QVERIFY2(sync.Text().contains(Greeting(SECOND)),
                 qPrintable(sync.Text()));
        QVERIFY2(sync.Text().contains(HIDDEN_TEXT), qPrintable(sync.Text()));
        // And how it got there: through no open.
        QVERIFY(RoundTrip());
        QVERIFY2(!Changes_.join(", ").contains("opening"),
                 qPrintable(Changes_.join(", ")));
        QCOMPARE(Status(), QString("open"));
        QCOMPARE(Proxy_->OpenRequests(), opens);
        QCOMPARE(Attaches(), attaches);
        QCOMPARE(Api_->Terminals().Session(Key(SECOND)), hidden);
    }

    // roostd keeps a session warm when a window goes: the next window opens it
    // from the snapshot, and the program has attached once.
    void ARestartedWindowReattachesWithoutANewAttach() {
        FailOnWarnings();
        QVERIFY(LaunchDaemon());
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        ShowAndSettle(FIRST);
        FocusTerminalSlot();
        // Each key in a read of its own: the next goes once the program has
        // shown the one before.
        int total = 0;
        for (const char key : {'w', 'o', 'r', 'l', 'd'}) {
            QTest::keyClick(Window(), key);
            ++total;
            QTRY_VERIFY_WITH_TIMEOUT(
                ScreenNow().contains(QStringLiteral("total=%1").arg(total)),
                TIMEOUT_MS);
        }
        QVERIFY(ScreenNow().contains(TYPED_WORLD));
        QCOMPARE(Attaches(), 1);

        LoseWindow();
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        Select(Ref(FIRST));

        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(TYPED_WORLD), TIMEOUT_MS);
        QVERIFY(ScreenNow().contains(Greeting(FIRST)));
        QCOMPARE(Attaches(), 1);
        // One open of the session, from the snapshot.
        QVERIFY(RoundTrip());
        QCOMPARE(Proxy_->OpenRequests(), 1);
    }
};

QTEST_MAIN(WarmSwitchTest)
#include "warm_switch_test.moc"
