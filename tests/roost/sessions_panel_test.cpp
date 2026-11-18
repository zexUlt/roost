// The built-in plugin `sessions`: its panel sessions.list in the shell's window
// (Main.qml of module Roost.Shell), against the real roostd with the real
// Claude plugin behind a fake `claude` whose `agents` prints a file and whose
// `attach` runs fake_tui, and with all the built-in plugins. What the user
// sees is read from the panels: the list's cursor, its rows, the status text,
// and the status and screen of the terminal; keys go through the window, to
// whatever has the focus.

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
#include <QQmlExpression>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <signal.h>

#include <algorithm>
#include <memory>

#include "api/api.hpp"
#include "core/rpc_client.hpp"
#include "qml_api/roost_singleton.hpp"
#include "qml_api/sessions_model.hpp"
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
const QString EMPTY_TEXT = "No session. Choose one in the list.";

// The sessions of tests/fixtures/claude_agents.json that `claude agents` lists,
// in the order roostd answers.
const QString ALPHA = "bg-0a1b2c3d";
const QString BETA = "bg-1b2c3d4e";
const QString GAMMA = "bg-2c3d4e5f";
const QString DELTA = "bg-3d4e5f60";
const QStringList ORDER = {ALPHA, BETA, GAMMA, DELTA};

// Each query answered once; a digit after the 1 would be more than once.
const QRegularExpression ANSWERED_ONCE(
    QStringLiteral(R"(answers da1=1 cpr=1(?!\d))"));

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

// A `claude` that cannot list.
constexpr const char* BROKEN_CLAUDE = R"sh(#!/bin/sh
echo "claude is broken" >&2
exit 2
)sh";

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

QByteArray ReadFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

}  // namespace

class SessionsPanelTest : public QObject {
    Q_OBJECT

private:
    QString SocketPath() const { return Dir_->filePath("roostd.sock"); }
    QString AgentsPath() const { return Dir_->filePath("bin/agents.json"); }

    bool WriteFakeClaude(const char* script) const {
        const QDir bin(Dir_->filePath("bin"));
        QFile file(bin.filePath("claude"));
        if (!bin.mkpath(".") || !file.open(QIODevice::WriteOnly) ||
            file.write(script) < 0 ||
            !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                                 QFile::ExeOwner)) {
            return false;
        }
        file.close();
        return QFile::copy(QStringLiteral(FIXTURES_DIR "/claude_agents.json"),
                           AgentsPath()) &&
               QFile::link(FAKE_TUI_PATH, bin.filePath("fake_tui"));
    }

    // `claude agents` lists the sessions of the fixture but those in `gone`;
    // the file is replaced whole, so that no run reads half of it.
    bool ListWithout(const QStringList& gone) const {
        QJsonArray left;
        for (const QJsonValue& entry :
             QJsonDocument::fromJson(
                 ReadFile(QStringLiteral(FIXTURES_DIR "/claude_agents.json")))
                 .array()) {
            if (!gone.contains(entry.toObject().value("id").toString())) {
                left.append(entry);
            }
        }
        QSaveFile agents(AgentsPath());
        return agents.open(QIODevice::WriteOnly) &&
               agents.write(
                   QJsonDocument(left).toJson(QJsonDocument::Compact)) >= 0 &&
               agents.commit();
    }

    bool StartDaemon() {
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("ROOST_SOCKET", SocketPath());
        environment.insert("ROOST_CONFIG", Dir_->filePath("config"));
        environment.insert("ROOST_PLUGINS_PATH", ROOST_PLUGINS_DIR);
        environment.insert(
            "PATH", Dir_->filePath("bin") + u':' + environment.value("PATH"));
        Daemon_ = std::make_unique<QProcess>();
        Daemon_->setProcessEnvironment(environment);
        Daemon_->setProcessChannelMode(QProcess::ForwardedErrorChannel);
        Daemon_->start(ROOSTD_PATH, {});
        return Daemon_->waitForStarted(TIMEOUT_MS) && Greets(SocketPath());
    }

    // roostd ends the way it does at a SIGTERM.
    void StopDaemon() {
        if (Daemon_ && Daemon_->state() != QProcess::NotRunning) {
            QCOMPARE(::kill(static_cast<pid_t>(Daemon_->processId()), SIGTERM),
                     0);
            QVERIFY(Daemon_->waitForFinished(TIMEOUT_MS));
            QCOMPARE(Daemon_->exitStatus(), QProcess::NormalExit);
            QCOMPARE(Daemon_->exitCode(), 0);
        }
        Daemon_.reset();
    }

    // The window of the shell, connected to the daemon, with the panels
    // loaded; waits until both slots hold theirs and the list has its rows
    // (`rows`, none when the sessions cannot be listed or `empty` says the
    // list starts without any).
    void Boot(int rows = 4, const char* claude = FAKE_CLAUDE,
              bool empty = false) {
        QVERIFY(WriteFakeClaude(claude));
        if (empty) {
            QVERIFY(ListWithout(ORDER));
        }
        QVERIFY(StartDaemon());
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
        QTRY_VERIFY_WITH_TIMEOUT(Singleton_->sessions()->loaded(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Count(), rows, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), rows > 0 ? 0 : -1, TIMEOUT_MS);
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

    QQuickItem* ListView() {
        return PanelOf(LIST)->findChild<QQuickItem*>("sessionList");
    }

    int Count() { return ListView()->property("count").toInt(); }
    int Cursor() { return ListView()->property("currentIndex").toInt(); }

    QQuickItem* ListStatus() {
        return PanelOf(LIST)->findChild<QQuickItem*>("sessionsStatus");
    }

    QString ListStatusNow() {
        return ListStatus()->property("text").toString();
    }

    QString UnavailableNow() {
        return PanelOf(LIST)
            ->findChild<QQuickItem*>("sessionsUnavailable")
            ->property("text")
            .toString();
    }

    // The id of the row of the model, whatever the view shows.
    QString IdAt(int row) {
        SessionsModel* model = Singleton_->sessions();
        return model->data(model->index(row, 0), Qt::UserRole + 1).toString();
    }

    QString CursorId() { return IdAt(Cursor()); }

    // The ids of the model's rows, in its order.
    QStringList Ids() {
        QStringList ids;
        for (int row = 0; row < Singleton_->sessions()->rowCount(); ++row) {
            ids << IdAt(row);
        }
        return ids;
    }

    // The id of the session in the list's output `selected`; empty without one.
    QString SelectedId() {
        return PanelOf(LIST)
            ->property("selected")
            .toMap()
            .value("id")
            .toString();
    }

    QQuickItem* RowItem(int row) {
        QQuickItem* item = nullptr;
        QMetaObject::invokeMethod(ListView(), "itemAtIndex",
                                  Q_RETURN_ARG(QQuickItem*, item),
                                  Q_ARG(int, row));
        return item;
    }

    QString RowText(int row, const QString& objectName) {
        QQuickItem* item = RowItem(row);
        QQuickItem* text = item == nullptr
                               ? nullptr
                               : item->findChild<QQuickItem*>(objectName);
        return text == nullptr ? QString{} : text->property("text").toString();
    }

    TerminalItem* Terminal() {
        return PanelOf(TERMINAL)->findChild<TerminalItem*>("terminal");
    }

    QQuickItem* StatusText() {
        return PanelOf(TERMINAL)->findChild<QQuickItem*>("terminalStatus");
    }

    QString Status() { return Terminal()->status(); }

    // Records "<status>|<text>" at each change of the terminal's status, from
    // now on, so that a state that passes quickly is still seen.
    void Watch() {
        Changes_.clear();
        connect(Terminal(), &TerminalItem::statusChanged, this, [this] {
            Changes_ << Status() + "|" +
                            StatusText()->property("text").toString();
        });
    }

    bool Opened() const {
        return std::ranges::any_of(Changes_, [](const QString& change) {
            return change.startsWith("opening|");
        });
    }

    // What the session in the terminal shows now; empty without one.
    QString ScreenNow() {
        const auto* pane = Terminal()->findChild<SessionPane*>();
        return pane == nullptr ? QString{} : Screen(pane->Session());
    }

    // Waits until the session has had its queries answered.
    void Settle() {
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(ANSWERED_ONCE),
                                 TIMEOUT_MS);
    }

    // A tap at the centre of the row's delegate, through the window.
    void TapRow(int row) {
        QQuickItem* item = RowItem(row);
        QVERIFY(item != nullptr);
        QTest::mouseClick(
            Window(), Qt::LeftButton, Qt::NoModifier,
            item->mapToScene(QPointF(item->width() / 2, item->height() / 2))
                .toPoint());
    }

    void Press(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::keyClick(Window(), key, modifiers);
    }

    // Moves the cursor to the row and opens it with Return; waits until the
    // session is open in the terminal.
    void OpenRow(int row) {
        while (Cursor() < row) {
            Press(Qt::Key_Down);
        }
        while (Cursor() > row) {
            Press(Qt::Key_Up);
        }
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), row, TIMEOUT_MS);
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QCOMPARE(SelectedId(), IdAt(row));
        Settle();
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
        Engine_.reset();
        Singleton_ = nullptr;
        Api_.reset();
        StopDaemon();
    }

    void TheListPanelIsLoadedFromTheBuiltInPluginAndItsOutputIsSelected() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        QCOMPARE(PanelOf(LIST)->parentItem(), Slot(LIST));
        QCOMPARE(Slot(LIST)->property("linkOutput").toString(),
                 QString("selected"));
        QCOMPARE(Slot(TERMINAL)->property("linkInput").toString(),
                 QString("session"));
        QVERIFY(Problems().isEmpty());
        QVERIFY(ListView() != nullptr);
    }

    void TheRowsShowTheTitleAndTheStateInTheOrderOfRoostd() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        QCOMPARE(Ids(), ORDER);
        // The title is the name, or the id of a session without one.
        const QStringList titles = {"Fix the flaky test", "Review the PR",
                                    GAMMA, "Answer a question"};
        for (int row = 0; row < titles.size(); ++row) {
            QCOMPARE(RowText(row, "sessionTitle"), titles[row]);
        }
        QCOMPARE(RowText(0, "sessionDetail"), QString("working · /work/alpha"));
        QCOMPARE(RowText(1, "sessionDetail"), QString("blocked · /work/beta"));
        QCOMPARE(RowText(2, "sessionDetail"), QString("done · /work/gamma"));
        QVERIFY(ListStatusNow().isEmpty());
        QVERIFY(!ListStatus()->isVisible());
    }

    void TheKeysMoveTheCursorAndChangeNeitherTheSelectionNorTheTerminal() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(ListView()->hasActiveFocus());

        Press(Qt::Key_Down);
        QCOMPARE(Cursor(), 1);
        Press(Qt::Key_J);
        QCOMPARE(Cursor(), 2);
        Press(Qt::Key_Up);
        QCOMPARE(Cursor(), 1);
        Press(Qt::Key_K);
        QCOMPARE(Cursor(), 0);
        // The top and the bottom do not wrap.
        Press(Qt::Key_Up);
        QCOMPARE(Cursor(), 0);
        Press(Qt::Key_K);
        QCOMPARE(Cursor(), 0);
        for (int i = 0; i < 6; ++i) {
            Press(Qt::Key_Down);
        }
        QCOMPARE(Cursor(), 3);
        Press(Qt::Key_J);
        QCOMPARE(Cursor(), 3);
        // j with a modifier is not j.
        Press(Qt::Key_K, Qt::ControlModifier);
        Press(Qt::Key_K, Qt::ShiftModifier);
        QCOMPARE(Cursor(), 3);
        Press(Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(Cursor(), 3);
        Press(Qt::Key_Up);
        Press(Qt::Key_J, Qt::AltModifier);
        QCOMPARE(Cursor(), 2);

        // Moving opens nothing.
        QVERIFY(SelectedId().isEmpty());
        QCOMPARE(Status(), QString("none"));
        QCOMPARE(StatusText()->property("text").toString(), EMPTY_TEXT);
    }

    void ReturnOpensTheSessionUnderTheCursorInTheTerminal() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }

        Press(Qt::Key_Down);
        Press(Qt::Key_Return);

        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), BETA, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(BETA)),
                                 TIMEOUT_MS);

        // Enter of the keypad does the same, for the row above.
        Press(Qt::Key_Up);
        Press(Qt::Key_Enter, Qt::KeypadModifier);

        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), ALPHA, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(ALPHA)),
                                 TIMEOUT_MS);
        QVERIFY(!ScreenNow().contains(Greeting(BETA)));
        QCOMPARE(Status(), QString("open"));
    }

    void ATapOnARowOpensItLikeReturn() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(Cursor(), 0);

        TapRow(2);

        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), GAMMA, TIMEOUT_MS);
        QCOMPARE(Cursor(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(GAMMA)),
                                 TIMEOUT_MS);
    }

    void ReturnOnTheOpenRowNeitherReopensNorRewritesTheSelection() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        Watch();
        QSignalSpy changed(PanelOf(LIST), SIGNAL(selectedChanged()));

        Press(Qt::Key_Return);
        Press(Qt::Key_Enter, Qt::KeypadModifier);
        QTest::qWait(300);

        QCOMPARE(changed.count(), 0);
        QVERIFY2(!Opened(), qPrintable(Changes_.join(", ")));
        QCOMPARE(SelectedId(), BETA);
        QCOMPARE(Status(), QString("open"));
    }

    void ARemovedOpenSessionIsClearedAndTheCursorGoesToTheNextRow() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }

        QVERIFY(ListWithout({BETA}));

        QTRY_COMPARE_WITH_TIMEOUT(Count(), 3, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(SelectedId().isEmpty(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("none"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(StatusText()->isVisible(), TIMEOUT_MS);
        QCOMPARE(StatusText()->property("text").toString(), EMPTY_TEXT);
        QCOMPARE(Ids(), (QStringList{ALPHA, GAMMA, DELTA}));
        QCOMPARE(Cursor(), 1);
        QCOMPARE(CursorId(), GAMMA);
    }

    void ARemovedOtherSessionKeepsTheSelectionAndTheCursorOnItsSession() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        Watch();

        QVERIFY(ListWithout({ALPHA}));

        QTRY_COMPARE_WITH_TIMEOUT(Count(), 3, TIMEOUT_MS);
        // The cursor was on BETA, now the first row.
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), 0, TIMEOUT_MS);
        QCOMPARE(CursorId(), BETA);
        QCOMPARE(SelectedId(), BETA);
        // The terminal is not touched for some polls more.
        QTest::qWait(500);
        QCOMPARE(SelectedId(), BETA);
        QCOMPARE(Status(), QString("open"));
        QVERIFY2(!Opened(), qPrintable(Changes_.join(", ")));
        QVERIFY(ScreenNow().contains(Greeting(BETA)));
    }

    void NoSessionsLeftSaysSoAndReturnDoesNothing() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }

        QVERIFY(ListWithout(ORDER));

        QTRY_COMPARE_WITH_TIMEOUT(Count(), 0, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(ListStatusNow(), QString("No sessions."),
                                  TIMEOUT_MS);
        QVERIFY(ListStatus()->isVisible());
        QCOMPARE(Cursor(), -1);
        QTRY_VERIFY_WITH_TIMEOUT(SelectedId().isEmpty(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("none"), TIMEOUT_MS);

        // A TypeError would be a warning, which fails the test.
        Press(Qt::Key_Return);
        Press(Qt::Key_Enter, Qt::KeypadModifier);
        Press(Qt::Key_Down);
        Press(Qt::Key_J);
        QTest::qWait(100);
        QCOMPARE(Cursor(), -1);
        // `selected` is still null, not a ref of nothing.
        QVERIFY(PanelOf(LIST)->property("selected").isNull());
    }

    void ARefreshKeepsTheCursorAndTheOpenSessionAndOpensNothingAgain() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Down);
        Press(Qt::Key_Down);
        QCOMPARE(CursorId(), DELTA);
        Watch();

        // As a plugin asks: through the singleton, from the panel's context.
        QSignalSpy reset(Singleton_->sessions(),
                         &QAbstractItemModel::modelReset);
        QQmlExpression refresh(QQmlEngine::contextForObject(PanelOf(LIST)),
                               PanelOf(LIST), "Roost.sessions.refresh()");
        refresh.evaluate();
        QVERIFY2(!refresh.hasError(), qPrintable(refresh.error().toString()));
        QTRY_COMPARE_WITH_TIMEOUT(reset.count(), 1, TIMEOUT_MS);

        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), 3, TIMEOUT_MS);
        QCOMPARE(CursorId(), DELTA);
        QCOMPARE(SelectedId(), BETA);
        QCOMPARE(Status(), QString("open"));
        QVERIFY2(!Opened(), qPrintable(Changes_.join(", ")));
    }

    void ARestartedRoostdKeepsTheSelectionAndTheTerminalReopensTheSession() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Down);
        QCOMPARE(CursorId(), GAMMA);

        StopDaemon();

        QTRY_COMPARE_WITH_TIMEOUT(Root()->property("connection").toString(),
                                  QString("disconnected"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("disconnected"),
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(ListStatusNow(), QString("Loading sessions…"),
                                  TIMEOUT_MS);
        // Give a reconcile that is wrong the time to clear it.
        QTest::qWait(300);
        QCOMPARE(SelectedId(), BETA);

        QVERIFY(StartDaemon());

        QTRY_COMPARE_WITH_TIMEOUT(Root()->property("connection").toString(),
                                  QString("connected"), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(Count(), 4, TIMEOUT_MS);
        QCOMPARE(SelectedId(), BETA);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(BETA)),
                                 TIMEOUT_MS);
        // The cursor is where the user left it.
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), 2, TIMEOUT_MS);
    }

    void ARestartedRoostdThatLacksTheOpenSessionClearsTheSelection() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }

        StopDaemon();
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("disconnected"),
                                  TIMEOUT_MS);
        QVERIFY(ListWithout({BETA}));
        QVERIFY(StartDaemon());

        QTRY_COMPARE_WITH_TIMEOUT(Count(), 3, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(SelectedId().isEmpty(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("none"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(StatusText()->property("text").toString(),
                                  EMPTY_TEXT, TIMEOUT_MS);
        QCOMPARE(Cursor(), 0);
    }

    void ARestartedRoostdThatLacksTheCursorsSessionPutsTheCursorOnTheOpenRow() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenRow(1);
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Down);
        QCOMPARE(CursorId(), GAMMA);

        StopDaemon();
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("disconnected"),
                                  TIMEOUT_MS);
        QVERIFY(ListWithout({GAMMA}));
        QVERIFY(StartDaemon());

        QTRY_COMPARE_WITH_TIMEOUT(Count(), 3, 30000);
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), 1, TIMEOUT_MS);
        QCOMPARE(CursorId(), BETA);
        QCOMPARE(SelectedId(), BETA);
    }

    void AReturnBetweenTheResetAndTheReconcileOpensNothing() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Down);
        QCOMPARE(CursorId(), BETA);
        // Connected after the panel's own, so it runs once the panel has seen
        // the reset and before the panel's reconcile, which waits for the
        // event loop.
        int pressed = 0;
        connect(Singleton_->sessions(), &QAbstractItemModel::modelReset, this,
                [this, &pressed] {
                    ++pressed;
                    Press(Qt::Key_Return);
                });

        QQmlExpression refresh(QQmlEngine::contextForObject(PanelOf(LIST)),
                               PanelOf(LIST), "Roost.sessions.refresh()");
        refresh.evaluate();
        QVERIFY2(!refresh.hasError(), qPrintable(refresh.error().toString()));
        QTRY_COMPARE_WITH_TIMEOUT(pressed, 1, TIMEOUT_MS);
        QTest::qWait(300);

        QVERIFY(SelectedId().isEmpty());
        QCOMPARE(Status(), QString("none"));
        // The cursor is where the user left it, and Return works again.
        QCOMPARE(CursorId(), BETA);
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), BETA, TIMEOUT_MS);
    }

    void ACursorMovedByARemovalIsRememberedAcrossAReset() {
        FailOnWarnings();
        Boot();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Down);
        Press(Qt::Key_Down);
        QCOMPARE(CursorId(), GAMMA);

        QVERIFY(ListWithout({GAMMA}));
        QTRY_COMPARE_WITH_TIMEOUT(Count(), 3, TIMEOUT_MS);
        QCOMPARE(Cursor(), 2);
        QCOMPARE(CursorId(), DELTA);

        // The panel remembers the session the cursor is on now.
        QSignalSpy reset(Singleton_->sessions(),
                         &QAbstractItemModel::modelReset);
        QQmlExpression refresh(QQmlEngine::contextForObject(PanelOf(LIST)),
                               PanelOf(LIST), "Roost.sessions.refresh()");
        refresh.evaluate();
        QVERIFY2(!refresh.hasError(), qPrintable(refresh.error().toString()));
        QTRY_COMPARE_WITH_TIMEOUT(reset.count(), 1, TIMEOUT_MS);
        QTest::qWait(100);

        QCOMPARE(Cursor(), 2);
        QCOMPARE(CursorId(), DELTA);
    }

    void SessionsThatAppearInAnEmptyListPutTheCursorOnTheFirstRow() {
        FailOnWarnings();
        Boot(0, FAKE_CLAUDE, true);
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(ListStatusNow(), QString("No sessions."));

        QVERIFY(ListWithout({}));

        QTRY_COMPARE_WITH_TIMEOUT(Count(), 4, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), 0, TIMEOUT_MS);
        QVERIFY(ListStatusNow().isEmpty());
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), ALPHA, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(Greeting(ALPHA)),
                                 TIMEOUT_MS);
    }

    void BeforeTheFirstListThePanelSaysItIsLoading() {
        FailOnWarnings();
        // An API that is not connected: the model never loads.
        api::Api api;
        QQmlEngine engine;
        auto* singleton =
            engine.singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        QVERIFY(singleton != nullptr);
        singleton->setApi(api);
        QQmlComponent component(
            &engine, QUrl::fromLocalFile(QStringLiteral(
                         SESSIONS_PLUGIN_DIR "/qml/SessionsPanel.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        const std::unique_ptr<QObject> panel(component.create());
        QVERIFY(panel != nullptr);
        auto* text = panel->findChild<QQuickItem*>("sessionsStatus");
        QVERIFY(text != nullptr);

        QCOMPARE(text->property("text").toString(),
                 QString("Loading sessions…"));
        QVERIFY(text->isVisible());
    }

    void AnAgentThatCannotListIsSaidSoInsteadOfNoSessions() {
        FailOnWarnings();
        Boot(0, BROKEN_CLAUDE);
        if (QTest::currentTestFailed()) {
            return;
        }

        QTRY_VERIFY_WITH_TIMEOUT(
            UnavailableNow().startsWith("claude: claude agents exited with "
                                        "code 2"),
            TIMEOUT_MS);
        QVERIFY2(UnavailableNow().contains("claude is broken"),
                 qPrintable(UnavailableNow()));
        // Not "No sessions.": the list is empty because claude cannot list.
        QVERIFY(ListStatusNow().isEmpty());
        QVERIFY(!ListStatus()->isVisible());
        // The rows end above the text, so that it hides none of them.
        auto* unavailable =
            PanelOf(LIST)->findChild<QQuickItem*>("sessionsUnavailable");
        QVERIFY(unavailable->isVisible());
        QVERIFY(ListView()->y() + ListView()->height() <= unavailable->y());
    }

    // The build holds copies of the plugin's files (configure_file); a copy
    // that is older than the source is a panel the window does not show.
    void TheBuildCopiesOfThePluginAreTheSourceFiles() {
        for (const QString& file : {QStringLiteral("plugin.json"),
                                    QStringLiteral("qml/SessionsPanel.qml")}) {
            const QByteArray source =
                ReadFile(QStringLiteral(SESSIONS_PLUGIN_DIR "/") + file);
            const QByteArray copy =
                ReadFile(QStringLiteral(ROOST_PLUGINS_DIR "/sessions/") + file);
            QVERIFY2(!source.isEmpty(), qPrintable(file));
            QVERIFY2(source == copy,
                     qPrintable(file + ": the build copy is stale"));
        }
    }
};

QTEST_MAIN(SessionsPanelTest)
#include "sessions_panel_test.moc"
