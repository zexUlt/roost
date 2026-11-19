#include "helpers/sessions_window.hpp"

#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QQmlEngine>
#include <QSaveFile>
#include <QTest>

#include <signal.h>

#include <algorithm>

#include "core/rpc_client.hpp"
#include "helpers/window_teardown.hpp"
#include "qml_api/sessions_model.hpp"
#include "terminal/session_pane.hpp"

namespace roost::testing {

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

void SessionsWindow::Prepare() {
    Dir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(Dir_->isValid());
}

void SessionsWindow::Teardown() {
    CloseWindow();
    StopDaemon();
    WithoutTerminal_ = false;
    SpySelected_ = false;
}

bool SessionsWindow::WriteFakeClaude(const char* script) const {
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
bool SessionsWindow::ListWithout(const QStringList& gone) const {
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
           agents.write(QJsonDocument(left).toJson(QJsonDocument::Compact)) >=
               0 &&
           agents.commit();
}

// The plugins directory of the case: every built plugin but `claude` (and,
// when WithoutTerminal_, `terminal`) linked; and `claude` as the built
// manifest whose adapter is a script that runs the built one with a short
// --poll-ms. With the default of 3 s every change of the fake claude's list
// would cost a poll interval.
bool SessionsWindow::LinkPlugins() const {
    const QDir plugins(Dir_->filePath("plugins"));
    const QDir builtPlugins(QStringLiteral(ROOST_PLUGINS_DIR));
    if (!plugins.mkpath(".")) {
        return false;
    }
    for (const QString& id :
         builtPlugins.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (id == QStringLiteral("claude") ||
            (id == QStringLiteral("terminal") && WithoutTerminal_)) {
            continue;
        }
        // The daemon may be started again in a case: the links are there.
        if (!plugins.exists(id) &&
            !QFile::link(builtPlugins.filePath(id), plugins.filePath(id))) {
            return false;
        }
    }
    const QDir claude(plugins.filePath("claude"));
    const QString adapter =
        QStringLiteral(ROOST_PLUGINS_DIR "/claude/backend/claude-adapter");
    QFile built(QStringLiteral(ROOST_PLUGINS_DIR "/claude/plugin.json"));
    if (!claude.mkpath("backend") || !built.open(QIODevice::ReadOnly)) {
        return false;
    }
    // The manifest is kept as built; the script stands in for the program
    // it runs, so a manifest that runs another one is not one this helper
    // can speed up.
    const QByteArray manifest = built.readAll();
    const QJsonArray command = QJsonDocument::fromJson(manifest)
                                   .object()
                                   .value("backend")
                                   .toObject()
                                   .value("command")
                                   .toArray();
    if (command.isEmpty() || command.first().toString() !=
                                 QStringLiteral("backend/claude-adapter")) {
        return false;
    }
    const QByteArray shell =
        QStringLiteral("#!/bin/sh\nexec '%1' --poll-ms %2 \"$@\"\n")
            .arg(adapter)
            .arg(POLL_MS)
            .toUtf8();
    QFile script(claude.filePath("backend/claude-adapter"));
    QFile json(claude.filePath("plugin.json"));
    return script.open(QIODevice::WriteOnly) &&
           script.write(shell) == shell.size() &&
           script.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                                 QFile::ExeOwner) &&
           json.open(QIODevice::WriteOnly) &&
           json.write(manifest) == manifest.size();
}

bool SessionsWindow::StartDaemon() {
    if (!LinkPlugins()) {
        return false;
    }
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("ROOST_SOCKET", SocketPath());
    environment.insert("ROOST_CONFIG", Dir_->filePath("config"));
    environment.insert("ROOST_PLUGINS_PATH", Dir_->filePath("plugins"));
    environment.insert(
        "PATH", Dir_->filePath("bin") + u':' + environment.value("PATH"));
    Daemon_ = std::make_unique<QProcess>();
    Daemon_->setProcessEnvironment(environment);
    Daemon_->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    Daemon_->start(ROOSTD_PATH, {});
    return Daemon_->waitForStarted(TIMEOUT_MS) && Greets(SocketPath());
}

// roostd ends the way it does at a SIGTERM.
void SessionsWindow::StopDaemon() {
    if (Daemon_ && Daemon_->state() != QProcess::NotRunning) {
        QCOMPARE(::kill(static_cast<pid_t>(Daemon_->processId()), SIGTERM), 0);
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
void SessionsWindow::Boot(int rows, const char* claude, bool empty) {
    QVERIFY(WriteFakeClaude(claude));
    if (empty) {
        QVERIFY(ListWithout(ORDER));
    }
    QVERIFY(StartDaemon());
    OpenWindow();
    if (QTest::currentTestFailed()) {
        return;
    }
    QTRY_COMPARE_WITH_TIMEOUT(Count(), rows, TIMEOUT_MS);
    QTRY_COMPARE_WITH_TIMEOUT(Cursor(), rows > 0 ? 0 : NO_CURSOR, TIMEOUT_MS);
    // The list shows its rows before the stored document is read, and the
    // status says "Loading sessions…" until then; a document written
    // before the read is not seen by the panel (the store keeps no copy
    // of a document nobody has read, so it forwards no change of it).
    QTRY_VERIFY_WITH_TIMEOUT(PanelOf(LIST)
                                 ->findChild<QObject*>("sessionsModel")
                                 ->property("ready")
                                 .toBool(),
                             TIMEOUT_MS);
}

// A window of the shell, connected to the running daemon, with the panels
// loaded; waits until both slots hold theirs and the list has its first
// sessions.
void SessionsWindow::OpenWindow() {
    Api_ = std::make_unique<api::Api>();
    Engine_ = std::make_unique<QQmlApplicationEngine>();
    Singleton_ =
        Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
    QVERIFY(Singleton_ != nullptr);
    Singleton_->setApi(*Api_);
    Api_->Connect(SocketPath());
    Engine_->loadFromModule("Roost.Shell", "Main");
    QVERIFY(!Engine_->rootObjects().isEmpty());
    Loader_ =
        std::make_unique<PluginLoader>(*Engine_, Api_->Plugins(), *Singleton_);
    Shell_ = std::make_unique<Shell>(*Engine_->rootObjects().first(), *Loader_,
                                     *Api_, SocketPath());
    // One round of events at a time until the list panel is there, so that
    // the spy is on before the list and the document come.
    const QDeadlineTimer panelDeadline(TIMEOUT_MS);
    while (!WatchSelectedOnLoad() && !panelDeadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
    QTRY_VERIFY_WITH_TIMEOUT(
        PanelOf(LIST) != nullptr &&
            (WithoutTerminal_ || PanelOf(TERMINAL) != nullptr),
        TIMEOUT_MS);
    QTRY_COMPARE_WITH_TIMEOUT(Root()->property("connection").toString(),
                              QString("connected"), TIMEOUT_MS);
    auto* window = qobject_cast<QQuickWindow*>(Root());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QVERIFY(QTest::qWaitForWindowActive(window));
    QTRY_VERIFY_WITH_TIMEOUT(Singleton_->sessions()->loaded(), TIMEOUT_MS);
}

// Whether the list panel is loaded; with SpySelected_, the first time it
// is, every change of its `selected` is recorded, before any list is in.
bool SessionsWindow::WatchSelectedOnLoad() {
    QQuickItem* panel = PanelOf(LIST);
    if (panel != nullptr && SpySelected_ && !SelectedSpy_) {
        SelectedSpy_ =
            std::make_unique<QSignalSpy>(panel, SIGNAL(selectedChanged()));
    }
    return panel != nullptr;
}

// The window goes, the daemon stays: the shell before the loader, the
// loader before the engine, which holds the terminal that closes its
// session through the API.
void SessionsWindow::CloseWindow() {
    SelectedSpy_.reset();
    Shell_.reset();
    Loader_.reset();
    // The window stops rendering before it is destroyed.
    QuiesceWindows(Engine_ ? Engine_->rootObjects() : QObjectList{});
    Engine_.reset();
    Singleton_ = nullptr;
    Api_.reset();
}

QQuickItem* SessionsWindow::Slot(const QString& type) {
    for (auto* item : Root()->findChildren<QQuickItem*>("panelSlot")) {
        if (item->property("panelType").toString() == type) {
            return item;
        }
    }
    return nullptr;
}

QQuickItem* SessionsWindow::PanelOf(const QString& type) {
    QQuickItem* slot = Slot(type);
    return slot == nullptr ? nullptr
                           : slot->property("panel").value<QQuickItem*>();
}

QQuickItem* SessionsWindow::ListView() {
    return PanelOf(LIST)->findChild<QQuickItem*>("sessionList");
}

// The model of the list: its rows are headers and sessions.
QAbstractItemModel* SessionsWindow::List() {
    return PanelOf(LIST)->findChild<QAbstractItemModel*>("sessionsModel");
}

QVariant SessionsWindow::Role(int row, const QByteArray& name) {
    QAbstractItemModel* list = List();
    return list->data(list->index(row, 0),
                      list->roleNames().key(name, Qt::DisplayRole));
}

// The rows of the sessions, in the order of the list.
QList<int> SessionsWindow::SessionRows() {
    QList<int> rows;
    for (int row = 0; row < List()->rowCount(); ++row) {
        if (Role(row, "kind").toString() == "session") {
            rows << row;
        }
    }
    return rows;
}

// The number of the session under the cursor; NO_CURSOR without one,
// ON_HEADER on a header.
int SessionsWindow::Cursor() {
    const int row = CursorRow();
    if (row < 0) {
        return NO_CURSOR;
    }
    const auto at = SessionRows().indexOf(row);
    return at < 0 ? ON_HEADER : static_cast<int>(at);
}

void SessionsWindow::Invoke(const char* method, const QString& argument) {
    QVERIFY(
        QMetaObject::invokeMethod(List(), method, Q_ARG(QString, argument)));
}

// The kind of every row, "header" or "session", in the order of the list.
QStringList SessionsWindow::Kinds() {
    QStringList kinds;
    for (int row = 0; row < List()->rowCount(); ++row) {
        kinds << Role(row, "kind").toString();
    }
    return kinds;
}

// What the list stored for the plugin, as the daemon keeps it.
QJsonObject SessionsWindow::StoredDocument() {
    return QJsonDocument::fromJson(
               ReadFile(Dir_->filePath("config/sessions.json")))
        .object();
}

QQuickItem* SessionsWindow::ListStatus() {
    return PanelOf(LIST)->findChild<QQuickItem*>("sessionsStatus");
}

QString SessionsWindow::ListStatusNow() {
    return ListStatus()->property("text").toString();
}

QString SessionsWindow::UnavailableNow() {
    return PanelOf(LIST)
        ->findChild<QQuickItem*>("sessionsUnavailable")
        ->property("text")
        .toString();
}

// The id of the row of the model, whatever the view shows.
QString SessionsWindow::IdAt(int session) {
    return Role(ViewRow(session), "id").toString();
}

// The ids of the list's sessions, in its order.
QStringList SessionsWindow::Ids() {
    QStringList ids;
    for (int session = 0; session < Count(); ++session) {
        ids << IdAt(session);
    }
    return ids;
}

// The id of the session in the list's output `selected`; empty without one.
QString SessionsWindow::SelectedId() {
    return PanelOf(LIST)->property("selected").toMap().value("id").toString();
}

// The delegate of the view's row; of the session's number below.
QQuickItem* SessionsWindow::ViewItem(int viewRow) {
    QQuickItem* item = nullptr;
    QMetaObject::invokeMethod(ListView(), "itemAtIndex",
                              Q_RETURN_ARG(QQuickItem*, item),
                              Q_ARG(int, viewRow));
    return item;
}

QString SessionsWindow::RowText(int row, const QString& objectName) {
    QQuickItem* item = RowItem(row);
    QQuickItem* text =
        item == nullptr ? nullptr : item->findChild<QQuickItem*>(objectName);
    return text == nullptr ? QString{} : text->property("text").toString();
}

TerminalItem* SessionsWindow::Terminal() {
    return PanelOf(TERMINAL)->findChild<TerminalItem*>("terminal");
}

QQuickItem* SessionsWindow::StatusText() {
    return PanelOf(TERMINAL)->findChild<QQuickItem*>("terminalStatus");
}

// Records "<status>|<text>" at each change of the terminal's status, from
// now on, so that a state that passes quickly is still seen.
void SessionsWindow::Watch() {
    Changes_.clear();
    connect(Terminal(), &TerminalItem::statusChanged, this, [this] {
        Changes_ << Status() + "|" + StatusText()->property("text").toString();
    });
}

bool SessionsWindow::Opened() const {
    return std::ranges::any_of(Changes_, [](const QString& change) {
        return change.startsWith("opening|");
    });
}

// What the session in the terminal shows now; empty without one.
QString SessionsWindow::ScreenNow() {
    const auto* pane = Terminal()->findChild<SessionPane*>();
    return pane == nullptr ? QString{} : Screen(pane->Session());
}

// Waits until the session has had its queries answered.
void SessionsWindow::Settle() {
    QTRY_VERIFY_WITH_TIMEOUT(ScreenNow().contains(ANSWERED_ONCE), TIMEOUT_MS);
}

// A tap at the centre of the row's delegate, through the window.
void SessionsWindow::TapRow(int row) {
    QQuickItem* item = RowItem(row);
    QVERIFY(item != nullptr);
    QTest::mouseClick(
        Window(), Qt::LeftButton, Qt::NoModifier,
        item->mapToScene(QPointF(item->width() / 2, item->height() / 2))
            .toPoint());
}

void SessionsWindow::Press(Qt::Key key, Qt::KeyboardModifiers modifiers) {
    QTest::keyClick(Window(), key, modifiers);
}

// A press of a physical key on another layout: the Qt key and the text are
// the letter typed, and the event carries the native codes of both
// platforms (the macOS virtual key, the xkb keycode).
void SessionsWindow::PressNative(int key, const QString& text, quint32 mac,
                                 quint32 xkb, Qt::KeyboardModifiers modifiers) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers, xkb, mac, 0x100, text);
    event.setAccepted(false);
    QCoreApplication::sendEvent(Window(), &event);
}

// Moves the cursor to the row and opens it with Return; waits until the
// session is open in the terminal.
void SessionsWindow::OpenRow(int row) {
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

// Ctrl+\ from the terminal; waits until the list has the keyboard.
void SessionsWindow::BackToList() {
    Press(Qt::Key_Backslash, CONTROL_KEY);
    QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);
}

QStringList SessionsWindow::Problems() {
    return Root()->property("problems").toStringList();
}

// Auto-repeat of a key held down: the press arrives with `autorep` set.
void SessionsWindow::PressRepeated(Qt::Key key, Qt::KeyboardModifiers modifiers,
                                   const QString& text) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers, text, true);
    QVERIFY(QCoreApplication::sendEvent(Window(), &event));
}

bool SessionsWindow::PressAccepted(Qt::Key key,
                                   Qt::KeyboardModifiers modifiers) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    event.setAccepted(false);
    QCoreApplication::sendEvent(Window(), &event);
    return event.isAccepted();
}

// Until the panel has nothing left to write: it writes the document by
// itself after it is ready (the first read, then the open session once the
// list is in), one write at a time, the next one sent when the answer to
// the last comes. A write of a plugin made before that is stored between
// the panel's own, and the stale one that follows it comes back as a
// foreign change that replaces what the plugin wrote. `restored` is the
// last event that makes the panel write, so at most one write is in flight
// and one follows it. The answer to a request comes after those of the
// requests sent before it, on the one connection and in one order of
// delivery: so after the first round trip the follow-up is sent, and after
// the second its answer is in too.
void SessionsWindow::WaitForThePanelToStopWriting() {
    QTRY_VERIFY_WITH_TIMEOUT(PanelOf(LIST)->property("restored").toBool(),
                             TIMEOUT_MS);
    for (int round = 0; round < 2; ++round) {
        auto answered = std::make_shared<bool>(false);
        Api_->Io()
            .Request(QStringLiteral("store.get"),
                     QJsonObject{{QStringLiteral("plugin"),
                                  QStringLiteral("sessions")}})
            .then(this, [answered](rpc::Result) { *answered = true; });
        QTRY_VERIFY_WITH_TIMEOUT(*answered, TIMEOUT_MS);
    }
}

}  // namespace roost::testing
