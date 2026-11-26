// The search field and the form of a new session of the sessions panel in the
// shell's window: the keys that move the keyboard between the list, the field
// and the form, the draft of the form, and the start of a session through the
// command of the agent. The window, the daemon and the fake `claude` are
// helpers/sessions_window.hpp with a fake `claude` of its own that records what
// `--bg` is asked to do; the panels, the model and the plugin are the real
// ones. HOME is a temporary directory, and no real `claude` runs.
//
// With ROOST_TEST_SNAPSHOTS set to a directory, the cases save a picture of the
// window there, for a person to look at.

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickWindow>
#include <QSaveFile>
#include <QTest>

#include <optional>

#include "api/api.hpp"
#include "helpers/scoped_env.hpp"
#include "helpers/sessions_window.hpp"
#include "helpers/snapshot.hpp"
#include "qml_api/roost_singleton.hpp"
#include "qml_api/sessions_model.hpp"

using namespace roost;
using namespace roost::testing;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

// OpenForm() and ToName() end the case where they fail, so that one miss does
// not turn into a chain of failed checks that follow from it.
#define OPEN_FORM()                       \
    do {                                  \
        OpenForm();                       \
        if (QTest::currentTestFailed()) { \
            return;                       \
        }                                 \
    } while (false)
#define TO_NAME()                         \
    do {                                  \
        ToName();                         \
        if (QTest::currentTestFailed()) { \
            return;                       \
        }                                 \
    } while (false)

namespace {

// The keys of the sessions in the document `sessions`.
QString KeyOf(const QString& id) { return "claude/" + id; }

// The session that `claude --bg` starts: not in the fixture.
const QString STARTED = "bg-4e5f6071";

// A fake `claude`, steered by the files next to it: `agents` prints
// agents.json with the cwds of /work moved into this directory, which it
// makes, and adds a line to agents.polls, or, while agents.fail has content,
// fails with "claude is broken"; `attach` runs fake_tui; `--bg` adds
// a line to bg.runs, writes the directory it runs in and its arguments, each
// whole in brackets, to bg.ran, waits while bg.hold is there, kills its parent
// (the adapter) when bg.crash is there, fails with the exit code in bg.exit
// and the text of bg.stderr, else puts bg.agents in place of agents.json when
// it is there and prints bg.stdout.
constexpr const char* FORM_CLAUDE = R"sh(#!/bin/sh
dir=$(dirname "$0")
verb=$1
argv=; for a in "$@"; do argv="$argv[$a]"; done
if [ "$verb" = --bg ]; then verb=bg; fi
case "$verb" in
agents)
  echo poll >> "$dir/agents.polls"
  if [ -s "$dir/agents.fail" ]; then echo "claude is broken" >&2; exit 2; fi
  mkdir -p "$dir/work/alpha" "$dir/work/beta" "$dir/work/gamma" "$dir/work/delta" "$dir/work/other"
  sed 's|"/work/|"'"$dir"'/work/|g' "$dir/agents.json"
  ;;
attach)
  cd / && FAKE_TUI_GREETING=$2 exec "$dir/fake_tui"
  ;;
bg)
  echo run >> "$dir/bg.runs"
  printf '%s\n%s\n' "$(pwd -P)" "$argv" > "$dir/bg.ran"
  while [ -f "$dir/bg.hold" ]; do sleep 0.05; done
  if [ -f "$dir/bg.crash" ]; then kill -9 $PPID; sleep 5; exit 1; fi
  if [ -f "$dir/bg.exit" ]; then
    cat "$dir/bg.stderr" >&2
    exit $(cat "$dir/bg.exit")
  fi
  if [ -f "$dir/bg.agents" ]; then mv "$dir/bg.agents" "$dir/agents.json"; fi
  cat "$dir/bg.stdout"
  ;;
*) exit 2 ;;
esac
)sh";

}  // namespace

class SessionsFormTest : public SessionsWindow {
    Q_OBJECT

private:
    std::optional<ScopedEnv> Home_;
    QFont AppFont_;

    QString BinPath(const QString& name) const {
        return Dir_->filePath("bin/" + name);
    }

    bool Put(const QString& name, const QByteArray& content) const {
        QSaveFile file(BinPath(name));
        return file.open(QIODevice::WriteOnly) && file.write(content) >= 0 &&
               file.commit();
    }

    // The cwd and the arguments the fake claude was started with by --bg.
    QStringList BgRan() const {
        return QString::fromUtf8(ReadFile(BinPath("bg.ran")))
            .split('\n', Qt::SkipEmptyParts);
    }

    static QString Canonical(const QString& path) {
        return QFileInfo(path).canonicalFilePath();
    }

    // How many times the fake claude was asked to start a session.
    int Runs() const {
        return static_cast<int>(QString::fromUtf8(ReadFile(BinPath("bg.runs")))
                                    .split('\n', Qt::SkipEmptyParts)
                                    .size());
    }

    // The fake claude's start waits until this is called.
    void Release() { QVERIFY(QFile::remove(BinPath("bg.hold"))); }

    // How many times the fake claude was asked for the list of the agents.
    int Polls() const {
        return static_cast<int>(
            QString::fromUtf8(ReadFile(BinPath("agents.polls")))
                .split('\n', Qt::SkipEmptyParts)
                .size());
    }

    // How the backend of the second agent is: it lists (the given sessions),
    // answers the list with an error, is not there (no binary) or ends as it
    // starts.
    enum class EOtherBackend { Lists, FailsToList, NoBinary, ExitsAtStart };

    // A second agent, `other`, with the command `other.newSession`, behind
    // the fixture backend (the real roostd runs it as it runs the Claude
    // plugin). Added before Start. Its starts are counted in newSession.runs.
    bool AddOtherAgent(EOtherBackend backend = EOtherBackend::Lists,
                       const QJsonArray& sessions = {}) {
        const QDir dir(Dir_->filePath("plugins/other"));
        if (!dir.mkpath(".")) {
            return false;
        }
        if (backend != EOtherBackend::NoBinary &&
            !QFile::link(FIXTURE_BACKEND_PATH, dir.filePath("backend"))) {
            return false;
        }
        QJsonArray command{"backend"};
        if (backend == EOtherBackend::ExitsAtStart) {
            command.append("--crash-on-start");
        }
        const QJsonObject manifest{
            {"id", "other"},
            {"version", "1"},
            {"api", "0"},
            {"backend", QJsonObject{{"command", command}}},
            {"contributes",
             QJsonObject{{"agents", QJsonArray{QJsonObject{{"id", "other"}}}},
                         {"commands", QJsonArray{QJsonObject{
                                          {"id", "other.newSession"},
                                          {"title", "New other session"}}}}}},
        };
        QSaveFile file(dir.filePath("plugin.json"));
        return file.open(QIODevice::WriteOnly) &&
               file.write(QJsonDocument(manifest).toJson()) >= 0 &&
               file.commit() &&
               SetOtherList(backend == EOtherBackend::FailsToList, sessions);
    }

    // What the fixture backend of `other` answers to a list from now on (it
    // reads agent.json anew for each request): the sessions, or an error.
    bool SetOtherList(bool fails, const QJsonArray& sessions = {}) {
        const QJsonObject answer =
            fails ? QJsonObject{{"error",
                                 QJsonObject{{"code", -32000},
                                             {"message", "other is broken"}}}}
                  : QJsonObject{{"result", sessions}};
        const QJsonObject answers{
            {"agent.listSessions", QJsonObject{{"other", answer}}}};
        QSaveFile file(Dir_->filePath("plugins/other/agent.json"));
        return file.open(QIODevice::WriteOnly) &&
               file.write(QJsonDocument(answers).toJson()) >= 0 &&
               file.commit();
    }

    // The starts the second agent was asked for: {agent, args} each.
    QList<QJsonObject> OtherRuns() const {
        QList<QJsonObject> runs;
        for (const QByteArray& line :
             ReadFile(Dir_->filePath("plugins/other/newSession.runs"))
                 .split('\n')) {
            if (!line.isEmpty()) {
                runs.append(QJsonDocument::fromJson(line).object());
            }
        }
        return runs;
    }

    // The session of the second agent in its list.
    static QJsonObject OtherSession() {
        return {{"id", "o1"},
                {"title", "Other one"},
                {"cwd", "/tmp"},
                {"state", "idle"},
                {"startedAt", qint64{1790150400000}}};
    }

    // The font of the application, which the theme takes as its base, is a
    // state of the process: a case that changes it puts it back in cleanup().
    void ScaleAppFont(qreal factor) {
        QFont font = QGuiApplication::font();
        font.setPointSizeF(font.pointSizeF() * factor);
        QGuiApplication::setFont(font);
    }

    // What `claude agents` lists once STARTED is there.
    static QByteArray AgentsWithStarted() {
        QJsonArray all =
            QJsonDocument::fromJson(
                ReadFile(QStringLiteral(FIXTURES_DIR "/claude_agents.json")))
                .array();
        all.append(QJsonObject{{"id", STARTED},
                               {"cwd", "/work/alpha"},
                               {"kind", "background"},
                               {"startedAt", qint64{1790150400000}},
                               {"name", "Agent's own name"},
                               {"state", "working"},
                               {"status", "busy"}});
        return QJsonDocument(all).toJson(QJsonDocument::Compact);
    }

    // The fake claude starts STARTED when asked, and lists it afterwards.
    void ArmStart() {
        QVERIFY(Put("bg.stdout", "backgrounded · " + STARTED.toUtf8() +
                                     " · Agent's own name\n"));
        QVERIFY(Put("bg.agents", AgentsWithStarted()));
    }

    bool Seed(const QJsonObject& sections) {
        QJsonObject document = sections;
        document.insert("version", 1);
        if (!QDir().mkpath(Dir_->filePath("config"))) {
            return false;
        }
        QSaveFile file(Dir_->filePath("config/sessions.json"));
        return file.open(QIODevice::WriteOnly) &&
               file.write(QJsonDocument(document).toJson()) >= 0 &&
               file.commit();
    }

    // `withParts`: the panel must have the head and the form that the cases
    // drive; a case about what was there before the form goes without.
    // `empty`: `claude agents` lists no session, with `rows` 0.
    void Start(const QJsonObject& sections = {}, bool withParts = true,
               int rows = 4, const char* claude = FORM_CLAUDE,
               bool empty = false) {
        if (!sections.isEmpty()) {
            QVERIFY(Seed(sections));
        }
        Boot(rows, claude, empty);
        if (QTest::currentTestFailed() || !withParts) {
            return;
        }
        // A panel without the parts fails here, not on a null pointer.
        for (const char* name :
             {"sessionsSearch", "sessionsSearchInput", "sessionsNewButton",
              "sessionsNewForm", "sessionsNewTask", "sessionsNewName",
              "sessionsNewAgent", "sessionsNewCategory", "sessionsNewCreate",
              "sessionsNewBack"}) {
            QVERIFY2(Part(name) != nullptr, name);
        }
    }

    QQuickItem* Part(const QString& name) {
        return PanelOf(LIST)->findChild<QQuickItem*>(name);
    }

    QString TextOf(const QString& name) {
        return Part(name)->property("text").toString();
    }

    QQuickItem* Search() { return Part("sessionsSearchInput"); }
    QQuickItem* Form() { return Part("sessionsNewForm"); }
    QQuickItem* TaskField() { return Part("sessionsNewTask"); }
    QQuickItem* NameField() { return Part("sessionsNewName"); }
    QQuickItem* AgentBox() { return Part("sessionsNewAgent"); }
    QQuickItem* CategoryBox() { return Part("sessionsNewCategory"); }
    QQuickItem* CategoryName() { return Part("sessionsNewCategoryName"); }
    QQuickItem* CreateButton() { return Part("sessionsNewCreate"); }
    QQuickItem* BackLink() { return Part("sessionsNewBack"); }

    bool FormOpen() { return PanelOf(LIST)->property("newOpen").toBool(); }

    QString Query() { return List()->property("query").toString(); }

    QString Notice() {
        return Part("sessionsNotice")->isVisible() ? TextOf("sessionsNotice")
                                                   : QString();
    }

    // Why the start failed, as the form shows it next to Create.
    QString Problem() {
        const QQuickItem* const problem = Part("sessionsNewProblem");
        return problem != nullptr && problem->isVisible()
                   ? problem->property("text").toString()
                   : QString();
    }

    void Type(const QString& text) {
        for (const QChar c : text) {
            QTest::sendKeyEvent(QTest::Click, Window(),
                                static_cast<Qt::Key>(c.toUpper().unicode()),
                                QString(c), Qt::NoModifier);
        }
    }

    void Tab() { Press(Qt::Key_Tab); }
    void BackTab() { Press(Qt::Key_Backtab, Qt::ShiftModifier); }

    void Click(QQuickItem* item) {
        ClickAt(item, item->width() / 2, item->height() / 2);
    }

    // The item is shown, has a size and its centre lies inside the window. An
    // item whose layout has not run yet sits at the origin of its parent with
    // no size, and a click at its centre lands on something else.
    bool Placed(const QQuickItem* item) {
        const QRectF window(QPointF(0, 0), QSizeF(Window()->size()));
        return item->isVisible() && item->width() > 0 && item->height() > 0 &&
               window.contains(item->mapToScene(
                   QPointF(item->width() / 2, item->height() / 2)));
    }

    // The layouts run in the polish of a frame, before it is drawn: when a
    // frame that was asked for after the call has been swapped, every layout
    // that was pending at the call has run.
    void WaitForFrame() {
        QSignalSpy swapped(Window(), &QQuickWindow::frameSwapped);
        Window()->update();
        QVERIFY(!swapped.isEmpty() || swapped.wait(TIMEOUT_MS));
    }

    // A click at a point of the item, in its own coordinates, once the item
    // has its place.
    void ClickAt(QQuickItem* item, qreal x, qreal y) {
        QTRY_VERIFY_WITH_TIMEOUT(Placed(item), TIMEOUT_MS);
        WaitForFrame();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(Placed(item), TIMEOUT_MS);
        QTest::mouseClick(Window(), Qt::LeftButton, Qt::NoModifier,
                          item->mapToScene(QPointF(x, y)).toPoint());
    }

    // "+ New", then waits until the task has the keyboard. With the form open
    // already, the button is hidden and there is nothing to click.
    void OpenForm() {
        if (!FormOpen()) {
            Click(Part("sessionsNewButton"));
        }
        QTRY_VERIFY_WITH_TIMEOUT(FormOpen(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(TaskField()->hasActiveFocus(), TIMEOUT_MS);
    }

    // Moves the keyboard to the name and waits for it.
    void ToName() {
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(NameField()->hasActiveFocus(), TIMEOUT_MS);
    }

    // From the name to the category: through the agent row, which has the
    // keyboard on the way.
    void ToCategory() {
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(AgentBox()->hasActiveFocus(), TIMEOUT_MS);
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(CategoryBox()->hasActiveFocus(), TIMEOUT_MS);
    }

    static QObject* PopupOf(QQuickItem* box) {
        return qvariant_cast<QObject*>(box->property("popup"));
    }

    QObject* CategoryPopup() { return PopupOf(CategoryBox()); }

    // Opens the list of the box that has the keyboard with Space, walks it
    // with Down or Up to the entry at `index` and takes it with Enter: an
    // explicit choice, unlike a Down on the closed box.
    void PickWithKeys(QQuickItem* box, int index) {
        Press(Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(PopupOf(box)->property("visible").toBool(),
                                 TIMEOUT_MS);
        for (int step = 0;
             step < 8 && box->property("highlightedIndex").toInt() != index;
             ++step) {
            Press(box->property("highlightedIndex").toInt() < index
                      ? Qt::Key_Down
                      : Qt::Key_Up);
        }
        QCOMPARE(box->property("highlightedIndex").toInt(), index);
        Press(Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!PopupOf(box)->property("visible").toBool(),
                                 TIMEOUT_MS);
    }

    void PickCategoryWithKeys(int index) { PickWithKeys(CategoryBox(), index); }

    // The entries of the agent list, as it shows them, and the one it shows.
    QStringList AgentChoices() {
        return AgentBox()->property("model").toStringList();
    }
    QString CurrentAgent() {
        return AgentBox()->property("currentText").toString();
    }

    // The agent under the cursor of the list.
    QString CursorAgent() {
        return Role(ViewRow(Cursor()), "agent").toString();
    }

    // The agent the view of the panel stored as the last one chosen in the
    // form: the view of the panel is one member of `view` in the document.
    QString StoredNewAgent() {
        const QJsonObject views = StoredDocument().value("view").toObject();
        for (auto it = views.begin(); it != views.end(); ++it) {
            const QString agent =
                it.value().toObject().value("newAgent").toString();
            if (!agent.isEmpty()) {
                return agent;
            }
        }
        return {};
    }

    // The window is a new one: its list has read the document and settled.
    void WaitForTheListToBeReady() {
        QTRY_VERIFY_WITH_TIMEOUT(PanelOf(LIST)
                                     ->findChild<QObject*>("sessionsModel")
                                     ->property("ready")
                                     .toBool(),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!PanelOf(LIST)->property("resetting").toBool(),
                                 TIMEOUT_MS);
    }

    // "New category…" (the last entry) chosen in the list, the keyboard in
    // the box: the keyboard goes to the field of the name.
    void ChooseNewCategory() {
        QTRY_VERIFY_WITH_TIMEOUT(CategoryBox()->hasActiveFocus(), TIMEOUT_MS);
        PickCategoryWithKeys(CategoryBox()->property("count").toInt() - 1);
        QTRY_VERIFY_WITH_TIMEOUT(CategoryName()->hasActiveFocus(), TIMEOUT_MS);
    }

    // The place of a control inside the form, in the coordinates of the form.
    QRectF InForm(QQuickItem* item) {
        return QRectF(item->mapToItem(Form(), QPointF(0, 0)),
                      QSizeF(item->width(), item->height()));
    }

    // The control has the keyboard and lies inside what the form shows: below
    // its top and above the hint.
    void VerifyShown(QQuickItem* item, const char* what) {
        QTRY_VERIFY2_WITH_TIMEOUT(item->hasActiveFocus(), what, TIMEOUT_MS);
        const qreal hint = Part("sessionsNewHint")->y();
        QTRY_VERIFY2_WITH_TIMEOUT(
            InForm(item).top() >= 0 && InForm(item).bottom() <= hint, what,
            TIMEOUT_MS);
    }

    // Every control of the form, as Tab reaches it, is inside the form when it
    // has the keyboard; Create and the link back included.
    void VerifyTabShowsEveryControl() {
        OPEN_FORM();
        Type("Ship it");
        VerifyShown(TaskField(), "task");
        Tab();
        VerifyShown(NameField(), "name");
        Tab();
        VerifyShown(AgentBox(), "agent");
        Tab();
        VerifyShown(CategoryBox(), "category");
        Tab();
        VerifyShown(CreateButton(), "create");
        Snap("scrolled-to-create");
        Tab();
        VerifyShown(BackLink(), "back");
        Tab();
        VerifyShown(TaskField(), "task again");
        BackTab();
        VerifyShown(BackLink(), "back again");
        BackTab();
        VerifyShown(CreateButton(), "create again");
        // The hint does not cover a control, and nothing is cut off.
        QVERIFY(InForm(CreateButton()).bottom() <=
                Part("sessionsNewHint")->y());
    }

    void VerifyListShown() {
        // Create closes the form when the agent has answered.
        QTRY_VERIFY_WITH_TIMEOUT(!FormOpen(), TIMEOUT_MS);
        QVERIFY(!Form()->isVisible());
        QVERIFY(Part("sessionsHead")->isVisible());
        QVERIFY(ListView()->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);
    }

    void Snap(const QString& step) { QVERIFY(SaveSnapshot(Window(), step)); }

    // A draft of every field: the task, the name, a new category.
    void FillTheDraft() {
        OPEN_FORM();
        Type("Ship it");
        TO_NAME();
        Type("Shipped");
        ToCategory();
        ChooseNewCategory();
        Type("Brand new");
    }

    // Create on the draft, and the form after a failure: open, the draft as it
    // was, the reason next to Create, Create usable again.
    void VerifyFailureKeepsTheDraft(const QString& reason) {
        Click(CreateButton());
        QTRY_VERIFY_WITH_TIMEOUT(Problem().contains(reason), TIMEOUT_MS);
        QVERIFY(FormOpen());
        QCOMPARE(TextOf("sessionsNewTask"), QString("Ship it"));
        QCOMPARE(TextOf("sessionsNewName"), QString("Shipped"));
        QVERIFY(CategoryName()->isVisible());
        QCOMPARE(TextOf("sessionsNewCategoryName"), QString("Brand new"));
        QCOMPARE(CategoryBox()->property("currentText").toString(),
                 QString("New category…"));
        QTRY_VERIFY_WITH_TIMEOUT(CreateButton()->property("enabled").toBool(),
                                 TIMEOUT_MS);
        QCOMPARE(TextOf("sessionsNewCreate"), QString("Create session"));
        // The footer does not say it a second time.
        QVERIFY(Notice().isEmpty());
        QVERIFY(Problem().startsWith("Not started: ") || Problem() == reason);
        Snap("1-failed");
    }

    // The agent that cannot list stays in the list, marked; chosen, it gives
    // its reason under Create, Create is off, and nothing starts.
    void VerifyUnavailableAgentIsMarkedAndRefused(const QString& reason) {
        QTRY_VERIFY_WITH_TIMEOUT(UnavailableNow().startsWith("other: "),
                                 TIMEOUT_MS);
        OPEN_FORM();
        Type("Ship it");
        QTRY_COMPARE_WITH_TIMEOUT(
            AgentChoices(), (QStringList{"claude", "other (unavailable)"}),
            TIMEOUT_MS);
        QVERIFY(Problem().isEmpty());
        QVERIFY(CreateButton()->property("enabled").toBool());
        TO_NAME();
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(AgentBox()->hasActiveFocus(), TIMEOUT_MS);

        PickWithKeys(AgentBox(), 1);

        QTRY_COMPARE_WITH_TIMEOUT(Problem(), "other is unavailable: " + reason,
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!CreateButton()->property("enabled").toBool(),
                                 TIMEOUT_MS);
        Snap("1-unavailable-agent");
        // Enter in the task does what Create does: nothing. The form asks
        // roostd for nothing, so that there is no answer to wait for.
        BackTab();
        BackTab();
        QTRY_VERIFY_WITH_TIMEOUT(TaskField()->hasActiveFocus(), TIMEOUT_MS);
        Press(Qt::Key_Return);
        QVERIFY(!PanelOf(LIST)->property("newStarting").toBool());
        QCOMPARE(TextOf("sessionsNewCreate"), QString("Create session"));
        QVERIFY(FormOpen());
        QCOMPARE(Runs(), 0);
        QVERIFY(OtherRuns().isEmpty());

        // An agent that can start gives the form back.
        Tab();
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(AgentBox()->hasActiveFocus(), TIMEOUT_MS);
        PickWithKeys(AgentBox(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(Problem().isEmpty(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(CreateButton()->property("enabled").toBool(),
                                 TIMEOUT_MS);
    }

    // The list is not polled: the agents are asked again when an event of an
    // adapter says so, or while one cannot list. The test says so.
    void ListAgainNow() { Singleton_->sessions()->refresh(); }

    // The entries of the agent row once the list is asked again: the claude
    // adapter polls on its own, and what its poll found comes to roostd's
    // answer to the next ask.
    bool ChoicesAfterListingAgain(const QStringList& expected) {
        ListAgainNow();
        return AgentChoices() == expected;
    }

    // The form of an `other`-and-claude pair with `other` the agent of the
    // form, as the cursor makes it (a session of `other` under it) or as a
    // pick does, with a task typed.
    void OpenFormWithOtherChosen(bool byPick) {
        if (byPick) {
            OPEN_FORM();
            Type("Ship it");
            TO_NAME();
            Tab();
            QTRY_VERIFY_WITH_TIMEOUT(AgentBox()->hasActiveFocus(), TIMEOUT_MS);
            QTRY_COMPARE_WITH_TIMEOUT(AgentChoices().size(), 2, TIMEOUT_MS);
            PickWithKeys(AgentBox(), 1);
        } else {
            for (int step = 0; step < 12 && CursorAgent() != "other"; ++step) {
                Press(Qt::Key_Down);
            }
            QCOMPARE(CursorAgent(), QString("other"));
            OPEN_FORM();
            Type("Ship it");
        }
        QTRY_COMPARE_WITH_TIMEOUT(CurrentAgent(), QString("other"), TIMEOUT_MS);
    }

    // Create, and the session is started by `other` and by nobody else.
    void VerifyOtherStartsTheSession() {
        QTRY_VERIFY_WITH_TIMEOUT(CreateButton()->property("enabled").toBool(),
                                 TIMEOUT_MS);
        Click(CreateButton());
        VerifyListShown();
        QTRY_COMPARE_WITH_TIMEOUT(OtherRuns().size(), 1, TIMEOUT_MS);
        QCOMPARE(OtherRuns().value(0).value("agent").toString(),
                 QString("other"));
        QCOMPARE(Runs(), 0);
    }

    // The list of the other agent fails and recovers: the model of the row
    // changes each time, and the row goes on showing the agent that was chosen.
    void VerifyChosenAgentStaysWhenAnotherListFailsAndRecovers(bool byPick) {
        OpenFormWithOtherChosen(byPick);
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("agents.fail", "1"));
        QTRY_VERIFY_WITH_TIMEOUT(
            ChoicesAfterListingAgain({"claude (unavailable)", "other"}),
            TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(CurrentAgent(), QString("other"), TIMEOUT_MS);
        QVERIFY(Problem().isEmpty());
        QVERIFY(CreateButton()->property("enabled").toBool());

        QVERIFY(Put("agents.fail", ""));
        QTRY_COMPARE_WITH_TIMEOUT(AgentChoices(),
                                  (QStringList{"claude", "other"}), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(CurrentAgent(), QString("other"), TIMEOUT_MS);
        QVERIFY(Problem().isEmpty());
        VerifyOtherStartsTheSession();
    }

    // The chosen agent cannot list for a while: the row marks it and says why,
    // Create is off; when it lists again the row shows it as it was chosen and
    // Create starts it.
    void VerifyChosenAgentBecomesUnavailableAndRecovers(bool byPick) {
        OpenFormWithOtherChosen(byPick);
        if (QTest::currentTestFailed()) {
            return;
        }
        const QJsonArray sessions =
            byPick ? QJsonArray{} : QJsonArray{OtherSession()};
        QVERIFY(SetOtherList(true, sessions));
        ListAgainNow();
        QTRY_COMPARE_WITH_TIMEOUT(
            AgentChoices(), (QStringList{"claude", "other (unavailable)"}),
            TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(CurrentAgent(),
                                  QString("other (unavailable)"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            Problem() == "other is unavailable: other is broken", TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!CreateButton()->property("enabled").toBool(),
                                 TIMEOUT_MS);

        QVERIFY(SetOtherList(false, sessions));
        QTRY_COMPARE_WITH_TIMEOUT(AgentChoices(),
                                  (QStringList{"claude", "other"}), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(CurrentAgent(), QString("other"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Problem().isEmpty(), TIMEOUT_MS);
        VerifyOtherStartsTheSession();
    }

private slots:
    void init() {
        AppFont_ = QGuiApplication::font();
        Prepare();
        QVERIFY(QDir().mkpath(Dir_->filePath("home")));
        Home_.emplace("HOME", Dir_->filePath("home").toUtf8());
    }

    void cleanup() {
        Teardown();
        Home_.reset();
        QGuiApplication::setFont(AppFont_);
    }

    void TheSearchFieldIsVisibleAndSlashMovesTheKeyboardToIt() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Part("sessionsSearch")->isVisible());
        QCOMPARE(Part("sessionsSearch")->height(), 40.0);
        QCOMPARE(TextOf("sessionsSearchPlaceholder"),
                 QString("Search sessions"));
        QVERIFY(ListView()->hasActiveFocus());

        Press(Qt::Key_Slash);

        QTRY_VERIFY_WITH_TIMEOUT(Search()->hasActiveFocus(), TIMEOUT_MS);
        QVERIFY(!ListView()->hasActiveFocus());
        // The "/" is a key of the list, not a character of the field.
        QCOMPARE(TextOf("sessionsSearchInput"), QString());

        Type("flaky");
        QTRY_COMPARE_WITH_TIMEOUT(Ids(), (QStringList{ALPHA}), TIMEOUT_MS);
        QCOMPARE(Query(), QString("flaky"));
        Snap("1-query");
    }

    void EscInTheFieldClearsTheQueryAndReturnsToTheList() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Slash);
        Type("flaky");
        QTRY_COMPARE_WITH_TIMEOUT(Ids(), (QStringList{ALPHA}), TIMEOUT_MS);

        Press(Qt::Key_Escape);

        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);
        QCOMPARE(Query(), QString());
        QCOMPARE(TextOf("sessionsSearchInput"), QString());
        QTRY_COMPARE_WITH_TIMEOUT(Ids(), ORDER, TIMEOUT_MS);
    }

    void DownInTheFieldKeepsTheQueryAndReturnsToTheList() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Slash);
        // Two sessions match, so that a Down that the list took would move
        // the cursor to the second one.
        Type("the");
        QTRY_COMPARE_WITH_TIMEOUT(Ids(), (QStringList{ALPHA, BETA}),
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(CursorId(), ALPHA, TIMEOUT_MS);

        Press(Qt::Key_Down);

        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);
        QCOMPARE(Query(), QString("the"));
        QCOMPARE(TextOf("sessionsSearchInput"), QString("the"));
        QCOMPARE(Ids(), (QStringList{ALPHA, BETA}));
        // Down was the key that left the field; the list did not take it.
        QCOMPARE(CursorId(), ALPHA);
    }

    void TabDoesNotLeaveTheSearchField() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Slash);
        Type("fl");

        QVERIFY(PressAccepted(Qt::Key_Tab));
        QVERIFY(PressAccepted(Qt::Key_Backtab, Qt::ShiftModifier));

        QVERIFY(Search()->hasActiveFocus());
        QCOMPARE(TextOf("sessionsSearchInput"), QString("fl"));
    }

    void NewOpensTheFormInPlaceOfTheList() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(!Form()->isVisible());
        QCOMPARE(TextOf("sessionsNewButton"), QString("+ New"));

        OPEN_FORM();

        QVERIFY(Form()->isVisible());
        QVERIFY(!Part("sessionsHead")->isVisible());
        QVERIFY(!ListView()->isVisible());
        QCOMPARE(TextOf("sessionsNewBack"), QString("← Sessions"));
        QCOMPARE(TextOf("sessionsNewTitle"), QString("New session"));
        QCOMPARE(TextOf("sessionsNewCreate"), QString("Create session"));
        QCOMPARE(TaskField()->height(), 38.0);
        QCOMPARE(NameField()->height(), 38.0);
        QCOMPARE(NameField()->property("placeholderText").toString(),
                 QString("Named by the agent"));
        QVERIFY(TaskField()->y() < NameField()->y());
        QVERIFY(NameField()->y() < AgentBox()->y());
        QVERIFY(AgentBox()->y() < CategoryBox()->y());
        QVERIFY(!CreateButton()->property("enabled").toBool());
        Snap("1-empty");
    }

    void TabAndBacktabWalkTheFieldsOfTheForm() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        OPEN_FORM();
        Type("Ship it");

        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(NameField()->hasActiveFocus(), TIMEOUT_MS);
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(AgentBox()->hasActiveFocus(), TIMEOUT_MS);
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(CategoryBox()->hasActiveFocus(), TIMEOUT_MS);
        Tab();
        // A task makes the button usable, so that Tab reaches it.
        QTRY_VERIFY_WITH_TIMEOUT(CreateButton()->hasActiveFocus(), TIMEOUT_MS);
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(BackLink()->hasActiveFocus(), TIMEOUT_MS);
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(TaskField()->hasActiveFocus(), TIMEOUT_MS);

        BackTab();
        QTRY_VERIFY_WITH_TIMEOUT(BackLink()->hasActiveFocus(), TIMEOUT_MS);
        BackTab();
        QTRY_VERIFY_WITH_TIMEOUT(CreateButton()->hasActiveFocus(), TIMEOUT_MS);
        BackTab();
        QTRY_VERIFY_WITH_TIMEOUT(CategoryBox()->hasActiveFocus(), TIMEOUT_MS);
        BackTab();
        QTRY_VERIFY_WITH_TIMEOUT(AgentBox()->hasActiveFocus(), TIMEOUT_MS);
        BackTab();
        QTRY_VERIFY_WITH_TIMEOUT(NameField()->hasActiveFocus(), TIMEOUT_MS);
        BackTab();
        QTRY_VERIFY_WITH_TIMEOUT(TaskField()->hasActiveFocus(), TIMEOUT_MS);
        QCOMPARE(TextOf("sessionsNewTask"), QString("Ship it"));
    }

    void ANewCategoryAddsItsFieldToTheTabOrder() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        OPEN_FORM();
        QVERIFY(!CategoryName()->isVisible());
        TO_NAME();
        ToCategory();

        // Down on the closed box takes the next entry, "New category…", and
        // the field of its name appears; the keyboard stays on the box, which
        // is only being walked through.
        Press(Qt::Key_Down);

        QTRY_VERIFY_WITH_TIMEOUT(CategoryName()->isVisible(), TIMEOUT_MS);
        QVERIFY(CategoryBox()->hasActiveFocus());
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(CategoryName()->hasActiveFocus(), TIMEOUT_MS);
        Type("Ops");
        BackTab();
        QTRY_VERIFY_WITH_TIMEOUT(CategoryBox()->hasActiveFocus(), TIMEOUT_MS);
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(CategoryName()->hasActiveFocus(), TIMEOUT_MS);
        QCOMPARE(TextOf("sessionsNewCategoryName"), QString("Ops"));
    }

    void EscCancelsTheFormAndDropsTheDraft() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        OPEN_FORM();
        Type("Half");

        Press(Qt::Key_Escape);

        VerifyListShown();
        QVERIFY(BgRan().isEmpty());
        // A form opened again starts empty.
        OPEN_FORM();
        QCOMPARE(TextOf("sessionsNewTask"), QString());
        QCOMPARE(TextOf("sessionsNewName"), QString());
    }

    void EscCancelsFromTheOtherControlsToo() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        OPEN_FORM();
        Type("x");
        TO_NAME();
        ToCategory();
        Press(Qt::Key_Escape);
        VerifyListShown();

        OPEN_FORM();
        Type("x");
        TO_NAME();
        Tab();
        Tab();
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(CreateButton()->hasActiveFocus(), TIMEOUT_MS);
        Press(Qt::Key_Escape);
        VerifyListShown();
    }

    void TheLinkBackCancelsTheForm() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        OPEN_FORM();
        Type("Half");

        Click(BackLink());

        VerifyListShown();
        QVERIFY(BgRan().isEmpty());
    }

    void TheDraftSurvivesTheKeyboardGoingToAnotherPanel() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        OPEN_FORM();
        Type("Ship it");
        TO_NAME();
        Type("Shipping");
        ToCategory();
        ChooseNewCategory();
        Type("Ops");

        // The keyboard goes to the terminal's panel and back to the list's.
        Terminal()->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(!CategoryName()->hasActiveFocus(), TIMEOUT_MS);
        QVERIFY(FormOpen());
        QCOMPARE(TextOf("sessionsNewTask"), QString("Ship it"));
        QCOMPARE(TextOf("sessionsNewName"), QString("Shipping"));
        PanelOf(LIST)->forceActiveFocus();

        QTRY_VERIFY_WITH_TIMEOUT(CategoryName()->hasActiveFocus(), TIMEOUT_MS);
        QVERIFY(FormOpen());
        QCOMPARE(TextOf("sessionsNewTask"), QString("Ship it"));
        QCOMPARE(TextOf("sessionsNewName"), QString("Shipping"));
        QCOMPARE(TextOf("sessionsNewCategoryName"), QString("Ops"));
        QVERIFY(CreateButton()->property("enabled").toBool());
        Snap("1-draft");
    }

    void CreateStartsTheSessionThroughTheCommandOfTheAgent() {
        FailOnWarnings();
        Start(QJsonObject{{"categories", QJsonObject{{KeyOf(BETA), "Ops"}}}});
        if (QTest::currentTestFailed()) {
            return;
        }
        ArmStart();
        QSignalSpy activated(PanelOf(LIST), SIGNAL(activated()));
        // BETA's category comes first: the cursor starts on it.
        for (int step = 0; step < 8 && CursorId() != ALPHA; ++step) {
            Press(Qt::Key_Down);
        }
        QCOMPARE(CursorId(), ALPHA);
        OPEN_FORM();
        Type("Ship it");
        TO_NAME();
        Type("Shipped");
        ToCategory();
        // The known category after "Uncategorized".
        Press(Qt::Key_Down);
        QTRY_COMPARE_WITH_TIMEOUT(
            CategoryBox()->property("currentText").toString(), QString("Ops"),
            TIMEOUT_MS);

        Click(CreateButton());

        // The form is gone and the keyboard is back in the list.
        VerifyListShown();
        // The command of the agent ran in the directory of the session under
        // the cursor, with the task as its prompt.
        QTRY_COMPARE_WITH_TIMEOUT(BgRan().size(), 2, TIMEOUT_MS);
        QCOMPARE(Canonical(BgRan().value(0)), Canonical(BinPath("work/alpha")));
        QCOMPARE(BgRan().value(1), QString("[--bg][--][Ship it]"));
        // The session is listed with the next list of the agent, under the
        // category, with the name of the form as its title, not the task;
        // nothing is opened.
        QTRY_VERIFY_WITH_TIMEOUT(Ids().contains(STARTED), TIMEOUT_MS);
        const int row = ViewRow(static_cast<int>(Ids().indexOf(STARTED)));
        QTRY_COMPARE_WITH_TIMEOUT(Role(row, "category").toString(),
                                  QString("Ops"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Role(row, "title").toString(),
                                  QString("Shipped"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Notice().contains("Agent's own name"),
                                 TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            StoredDocument().value("names").toObject().value(KeyOf(STARTED)),
            QJsonValue("Shipped"), TIMEOUT_MS);
        QVERIFY(SelectedId().isEmpty());
        QCOMPARE(activated.count(), 0);
        Snap("1-after-create");
    }

    void ANewCategoryIsCreatedWithEnterInItsField() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        ArmStart();
        OPEN_FORM();
        Type("Ship it");
        TO_NAME();
        ToCategory();
        ChooseNewCategory();
        Type("Brand new");

        Press(Qt::Key_Return);

        VerifyListShown();
        QTRY_COMPARE_WITH_TIMEOUT(BgRan().value(1),
                                  QString("[--bg][--][Ship it]"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Ids().contains(STARTED), TIMEOUT_MS);
        const int row = ViewRow(static_cast<int>(Ids().indexOf(STARTED)));
        QTRY_COMPARE_WITH_TIMEOUT(Role(row, "category").toString(),
                                  QString("Brand new"), TIMEOUT_MS);
    }

    void CreateWithATaskAndNoNameLeavesTheTitleToTheAgent() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        ArmStart();
        OPEN_FORM();
        Type("Ship it");
        // A task alone is enough.
        QTRY_VERIFY_WITH_TIMEOUT(CreateButton()->property("enabled").toBool(),
                                 TIMEOUT_MS);
        Snap("2-task-only");
        TO_NAME();
        // A blank name is no name.
        Type("  ");
        ToCategory();
        ChooseNewCategory();
        Type("Brand new");

        Press(Qt::Key_Return);

        VerifyListShown();
        QTRY_COMPARE_WITH_TIMEOUT(BgRan().value(1),
                                  QString("[--bg][--][Ship it]"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Ids().contains(STARTED), TIMEOUT_MS);
        const int row = ViewRow(static_cast<int>(Ids().indexOf(STARTED)));
        // The category is set before the rename would be asked for: once it
        // is in the document, the answer of the command was handled whole.
        QTRY_COMPARE_WITH_TIMEOUT(StoredDocument()
                                      .value("categories")
                                      .toObject()
                                      .value(KeyOf(STARTED)),
                                  QJsonValue("Brand new"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Notice().contains("Agent's own name"),
                                 TIMEOUT_MS);
        QCOMPARE(Role(row, "title").toString(), QString("Agent's own name"));
        QVERIFY(!StoredDocument().value("names").toObject().contains(
            KeyOf(STARTED)));
    }

    void NothingStartsWithoutATask() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        ArmStart();
        OPEN_FORM();
        QVERIFY(!CreateButton()->property("enabled").toBool());

        // A name is not enough.
        TO_NAME();
        Type("Shipping");
        QVERIFY(!CreateButton()->property("enabled").toBool());
        // Enter in the name, in an empty task and in a blank one starts
        // nothing; the command that would answer is armed.
        Press(Qt::Key_Return);
        BackTab();
        QTRY_VERIFY_WITH_TIMEOUT(TaskField()->hasActiveFocus(), TIMEOUT_MS);
        Press(Qt::Key_Return);
        Type("  ");
        Press(Qt::Key_Return);
        Click(CreateButton());

        // A start says "Starting…" at once, in the panel; the form is still
        // open and the button is as it was.
        QVERIFY(FormOpen());
        QVERIFY(!CreateButton()->property("enabled").toBool());
        QCOMPARE(TextOf("sessionsNewName"), QString("Shipping"));
        QVERIFY(Notice().isEmpty());
        // Barrier: two more polls of the agents, one of which started after
        // the last key. A command of the agent that the keys had started would
        // have been run, and listed, by then.
        const int polls = Polls();
        QTRY_VERIFY_WITH_TIMEOUT(Polls() >= polls + 2, TIMEOUT_MS);
        QVERIFY(BgRan().isEmpty());
        QVERIFY(!Ids().contains(STARTED));
        QVERIFY(Notice().isEmpty());
    }

    void NewWhileThePromptIsOpenTypesIntoTheTask() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_N);
        QTRY_VERIFY_WITH_TIMEOUT(Part("sessionsPromptInput")->hasActiveFocus(),
                                 TIMEOUT_MS);
        Type("half");

        Click(Part("sessionsNewButton"));

        QTRY_VERIFY_WITH_TIMEOUT(FormOpen(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(TaskField()->hasActiveFocus(), TIMEOUT_MS);
        // The prompt is gone with what was typed into it.
        QVERIFY(PanelOf(LIST)->property("promptKind").toString().isEmpty());
        QVERIFY(!Part("sessionsPrompt")->isVisible());
        Type("abc");
        // The letters are the task's: no key of the list took them (a would
        // switch the tab, c would open the prompt).
        QCOMPARE(TextOf("sessionsNewTask"), QString("abc"));
        QCOMPARE(List()->property("tab").toString(), QString("all"));
        QVERIFY(PanelOf(LIST)->property("promptKind").toString().isEmpty());
        QVERIFY(TaskField()->hasActiveFocus());

        Press(Qt::Key_Escape);
        VerifyListShown();
        QVERIFY(!Part("sessionsPrompt")->isVisible());
        QVERIFY(BgRan().isEmpty());
    }

    void NewWhileAConfirmationWaitsDropsItAndTypesIntoTheTask() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        for (int step = 0; step < 8 && Cursor() < 0; ++step) {
            Press(Qt::Key_Down);
        }
        QVERIFY(Cursor() >= 0);
        Press(Qt::Key_D);
        QVERIFY(Part("sessionsConfirm")->isVisible());

        Click(Part("sessionsNewButton"));

        QTRY_VERIFY_WITH_TIMEOUT(FormOpen(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(TaskField()->hasActiveFocus(), TIMEOUT_MS);
        // The question is not kept for the list that is hidden: a y in the
        // task would otherwise be the answer to it.
        QVERIFY(!Part("sessionsConfirm")->isVisible());
        Type("yes abc");
        QCOMPARE(TextOf("sessionsNewTask"), QString("yes abc"));
        QCOMPARE(List()->property("tab").toString(), QString("all"));
        QVERIFY(!Part("sessionsConfirm")->isVisible());

        Press(Qt::Key_Escape);
        VerifyListShown();
        QVERIFY(!Part("sessionsConfirm")->isVisible());
        QCOMPARE(Ids(), ORDER);
    }

    void AStartThatCannotBeAskedForKeepsTheUnavailableAgentsInView() {
        FailOnWarnings();
        Start({}, true, 0, BROKEN_CLAUDE);
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(!UnavailableNow().isEmpty(), TIMEOUT_MS);

        OPEN_FORM();

        // The form is in place of the list; the reason that no agent lists
        // is not hidden by it, and the form ends above it.
        auto* unavailable = Part("sessionsUnavailable");
        QVERIFY(unavailable->isVisible());
        QVERIFY(unavailable->property("text").toString().contains(
            "claude is broken"));
        QVERIFY(Form()->y() + Form()->height() <= unavailable->y());
        QVERIFY(Form()->isVisible());
        Snap("1-unavailable");
    }

    void TheCategoryBoxShowsTheCategoryThatCreateApplies() {
        FailOnWarnings();
        Start(QJsonObject{{"categories", QJsonObject{{KeyOf(BETA), "Ops"}}}});
        if (QTest::currentTestFailed()) {
            return;
        }
        ArmStart();
        // The cursor starts on the header "Ops": the form takes its category.
        QCOMPARE(List()->property("cursorCategory").toString(), QString("Ops"));
        OPEN_FORM();
        QCOMPARE(CategoryBox()->property("currentText").toString(),
                 QString("Ops"));
        Type("Ship it");

        // The only session of "Ops" leaves with the next list of the agent.
        QVERIFY(ListWithout({BETA}));
        QTRY_VERIFY_WITH_TIMEOUT(!Ids().contains(BETA), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            !List()->property("knownCategories").toStringList().contains("Ops"),
            TIMEOUT_MS);

        // The box still says what Create applies.
        QCOMPARE(CategoryBox()->property("currentText").toString(),
                 QString("Ops"));
        Click(CreateButton());

        VerifyListShown();
        QTRY_COMPARE_WITH_TIMEOUT(BgRan().value(1),
                                  QString("[--bg][--][Ship it]"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(StoredDocument()
                                      .value("categories")
                                      .toObject()
                                      .value(KeyOf(STARTED)),
                                  QJsonValue("Ops"), TIMEOUT_MS);
    }

    void AClickOnTheFrameOfTheSearchFieldAwayFromTheTextFocusesIt() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QQuickItem* frame = Part("sessionsSearch");
        QQuickItem* icon = Part("sessionsSearchIcon");
        QVERIFY(ListView()->hasActiveFocus());

        // On the icon.
        ClickAt(icon, icon->width() / 2, icon->height() / 2);
        QTRY_VERIFY_WITH_TIMEOUT(Search()->hasActiveFocus(), TIMEOUT_MS);
        Press(Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);

        // Over the line of the text, in the margin of the frame.
        ClickAt(frame, frame->width() / 2, 2);
        QTRY_VERIFY_WITH_TIMEOUT(Search()->hasActiveFocus(), TIMEOUT_MS);
        Press(Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);

        // At the right end, past the text.
        ClickAt(frame, frame->width() - 4, frame->height() / 2);
        QTRY_VERIFY_WITH_TIMEOUT(Search()->hasActiveFocus(), TIMEOUT_MS);
        Type("flaky");
        QTRY_COMPARE_WITH_TIMEOUT(Ids(), (QStringList{ALPHA}), TIMEOUT_MS);
    }

    void ALowPanelScrollsTheFormToTheControlThatHasTheKeyboard() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        Window()->setHeight(300);
        QTRY_VERIFY_WITH_TIMEOUT(PanelOf(LIST)->height() <= 300, TIMEOUT_MS);

        VerifyTabShowsEveryControl();
        if (QTest::currentTestFailed()) {
            return;
        }
        // The case means something only if the form does not fit: it was
        // scrolled to reach Create, and the hint is below the scrolled part.
        QQuickItem* scroll = Part("sessionsNewScroll");
        QVERIFY(scroll != nullptr);
        QVERIFY(scroll->property("contentHeight").toReal() > scroll->height());
        QVERIFY(scroll->property("contentY").toReal() > 0);
        QVERIFY(Part("sessionsNewHint")->isVisible());
        QVERIFY(scroll->y() + scroll->height() <= Part("sessionsNewHint")->y());
    }

    void TheFormGrowsWithALargeInterfaceFont() {
        FailOnWarnings();
        ScaleAppFont(2.5);
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        OPEN_FORM();

        // The controls are not lower than their text, and not lower than the
        // style says.
        for (QQuickItem* field : {TaskField(), NameField()}) {
            QVERIFY(field->height() >= 38);
            QVERIFY(field->height() >=
                    field->property("contentHeight").toReal() +
                        field->property("topPadding").toReal() +
                        field->property("bottomPadding").toReal());
        }
        QVERIFY(CategoryBox()->height() > 38);
        QVERIFY(CreateButton()->height() > 34);
        QVERIFY(CreateButton()->height() >=
                CreateButton()->property("implicitContentHeight").toReal() +
                    CreateButton()->property("topPadding").toReal() +
                    CreateButton()->property("bottomPadding").toReal());
        Snap("1-large-font");

        VerifyTabShowsEveryControl();
    }

    void ANewCategoryIsChosenInTheListAndTheKeyboardMovesToItsField() {
        FailOnWarnings();
        Start(QJsonObject{{"categories", QJsonObject{{KeyOf(BETA), "Ops"}}}});
        if (QTest::currentTestFailed()) {
            return;
        }
        OPEN_FORM();
        Type("Ship it");
        TO_NAME();
        ToCategory();
        // Arrows on the closed box walk the entries and keep the keyboard.
        Press(Qt::Key_Down);
        Press(Qt::Key_Down);
        QTRY_VERIFY_WITH_TIMEOUT(CategoryName()->isVisible(), TIMEOUT_MS);
        QVERIFY(CategoryBox()->hasActiveFocus());
        QVERIFY(!CreateButton()->property("enabled").toBool());
        // Back to a known category: no field, Create is usable again.
        Press(Qt::Key_Up);
        QTRY_VERIFY_WITH_TIMEOUT(!CategoryName()->isVisible(), TIMEOUT_MS);
        QVERIFY(CategoryBox()->hasActiveFocus());
        QCOMPARE(CategoryBox()->property("currentText").toString(),
                 QString("Ops"));
        QVERIFY(CreateButton()->property("enabled").toBool());

        // Chosen in the list that the box opens, with Enter on the entry.
        PickCategoryWithKeys(CategoryBox()->property("count").toInt() - 1);
        QTRY_VERIFY_WITH_TIMEOUT(CategoryName()->hasActiveFocus(), TIMEOUT_MS);
        QVERIFY(CategoryName()->isVisible());
    }

    void AClickOnTheNewCategoryEntryMovesTheKeyboardToItsField() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        OPEN_FORM();
        TO_NAME();
        ToCategory();

        Click(CategoryBox());
        QTRY_VERIFY_WITH_TIMEOUT(CategoryPopup()->property("visible").toBool(),
                                 TIMEOUT_MS);
        const int last = CategoryBox()->property("count").toInt() - 1;
        QQuickItem* entry = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (entry =
                 [&] {
                     QQuickItem* content = qvariant_cast<QQuickItem*>(
                         CategoryPopup()->property("contentItem"));
                     QQuickItem* item = nullptr;
                     QMetaObject::invokeMethod(content, "itemAtIndex",
                                               Q_RETURN_ARG(QQuickItem*, item),
                                               Q_ARG(int, last));
                     return item;
                 }()) != nullptr,
            TIMEOUT_MS);
        Click(entry);

        QTRY_VERIFY_WITH_TIMEOUT(CategoryName()->hasActiveFocus(), TIMEOUT_MS);
    }

    void NoAgentThatCanStartKeepsTheDraft() {
        FailOnWarnings();
        WithoutCommands_ = {"claude.newSession"};
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        FillTheDraft();
        VerifyFailureKeepsTheDraft("No agent can start a session");
        QCOMPARE(Runs(), 0);
    }

    void ARefusedStartKeepsTheDraft() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("bg.exit", "1"));
        QVERIFY(Put("bg.stderr", "Workspace not trusted. Run it by hand.\n"));
        FillTheDraft();
        VerifyFailureKeepsTheDraft("Workspace not trusted");
        QVERIFY(Problem().startsWith("Not started: "));
        QCOMPARE(Runs(), 1);

        // The same draft starts once the cause is gone.
        QVERIFY(QFile::remove(BinPath("bg.exit")));
        ArmStart();
        Click(CreateButton());
        VerifyListShown();
        QTRY_VERIFY_WITH_TIMEOUT(Ids().contains(STARTED), TIMEOUT_MS);
        QCOMPARE(Runs(), 2);
    }

    void AStartThatTimesOutKeepsTheDraft() {
        FailOnWarnings();
        // The adapter's own limit of a start: the window has none.
        AdapterArgs_ = {"--new-session-timeout-ms", "300"};
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("bg.hold", ""));
        FillTheDraft();
        VerifyFailureKeepsTheDraft("did not finish in 300 ms");
    }

    void AnAnswerWithoutAnIdKeepsTheDraft() {
        FailOnWarnings();
        AnswerWithoutSessionId_ = true;
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        ArmStart();
        FillTheDraft();
        VerifyFailureKeepsTheDraft("answered without a session id");
        QCOMPARE(Runs(), 1);
    }

    void ABackendThatExitsDuringTheStartKeepsTheDraft() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("bg.crash", ""));
        FillTheDraft();
        VerifyFailureKeepsTheDraft("the backend of claude");
        // The window and roostd are alive, and roostd answers.
        QVERIFY(Window()->isVisible());
        QCOMPARE(Daemon_->state(), QProcess::Running);
        WAIT_FOR_EFFECTS();
        QVERIFY(FormOpen());
    }

    void CreateWaitsForTheAnswerAndASecondStartIsRefused() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        ArmStart();
        QVERIFY(Put("bg.hold", ""));
        FillTheDraft();

        Click(CreateButton());

        QTRY_COMPARE_WITH_TIMEOUT(Runs(), 1, TIMEOUT_MS);
        // The start is under way: the form is open, Create is off and says so.
        QVERIFY(FormOpen());
        QTRY_VERIFY_WITH_TIMEOUT(!CreateButton()->property("enabled").toBool(),
                                 TIMEOUT_MS);
        QCOMPARE(TextOf("sessionsNewCreate"), QString("Starting…"));
        QVERIFY(Problem().isEmpty());
        // Another click and Enter start nothing.
        Click(CreateButton());
        Press(Qt::Key_Return);
        QVERIFY(FormOpen());

        Release();
        VerifyListShown();
        QTRY_VERIFY_WITH_TIMEOUT(Ids().contains(STARTED), TIMEOUT_MS);
        // Barrier: two more polls of the agents, one of which began after the
        // second click and the Enter; a start they had made would have run.
        const int polls = Polls();
        QTRY_VERIFY_WITH_TIMEOUT(Polls() >= polls + 2, TIMEOUT_MS);
        QCOMPARE(Runs(), 1);
    }

    void CancelWhileStartingClosesTheFormAndAFailureComesToTheFooter() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("bg.exit", "1"));
        QVERIFY(Put("bg.stderr", "Workspace not trusted. Run it by hand.\n"));
        QVERIFY(Put("bg.hold", ""));
        FillTheDraft();
        // Create with the keyboard: the button that has it goes off, and Esc
        // must still reach the form.
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(CreateButton()->hasActiveFocus(), TIMEOUT_MS);
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(Runs(), 1, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!CreateButton()->property("enabled").toBool(),
                                 TIMEOUT_MS);

        Press(Qt::Key_Escape);
        VerifyListShown();
        Release();

        QTRY_VERIFY_WITH_TIMEOUT(Notice().startsWith("Not started: "),
                                 TIMEOUT_MS);
        QVERIFY(Notice().contains("Workspace not trusted"));
        // The form did not come back for it.
        QVERIFY(!FormOpen());
    }

    void CancelWhileStartingStillListsASessionThatStarts() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        ArmStart();
        QVERIFY(Put("bg.hold", ""));
        FillTheDraft();
        Click(CreateButton());
        QTRY_COMPARE_WITH_TIMEOUT(Runs(), 1, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!CreateButton()->property("enabled").toBool(),
                                 TIMEOUT_MS);

        Click(BackLink());
        VerifyListShown();
        Release();

        QTRY_VERIFY_WITH_TIMEOUT(Ids().contains(STARTED), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Notice().contains("Agent's own name"),
                                 TIMEOUT_MS);
        QVERIFY(!FormOpen());
    }

    void ALateSuccessDoesNotTakeTheKeyboardFromAnotherPanel() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        ArmStart();
        QVERIFY(Put("bg.hold", ""));
        FillTheDraft();
        Click(CreateButton());
        QTRY_COMPARE_WITH_TIMEOUT(Runs(), 1, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!CreateButton()->property("enabled").toBool(),
                                 TIMEOUT_MS);

        // The user goes to the terminal while the agent starts.
        Terminal()->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(Terminal()->hasActiveFocus(), TIMEOUT_MS);
        QVERIFY(FormOpen());
        Release();

        QTRY_VERIFY_WITH_TIMEOUT(Ids().contains(STARTED), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!FormOpen(), TIMEOUT_MS);
        WAIT_FOR_EFFECTS();
        // The keys the user types now go to the terminal, not to the list.
        QVERIFY(Terminal()->hasActiveFocus());
        QVERIFY(!ListView()->hasActiveFocus());
        // The list is what the panel gives the keyboard to when it gets it.
        QVERIFY(ListView()->property("focus").toBool());
        QVERIFY(ListView()->isVisible());
    }

    void AReasonIsWrappedByWordsAtTheLeastWidthOfTheList() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        const QString reason =
            "Workspace not trusted. Run it by hand once, accept the trust "
            "dialog of the folder, and start the session again from here, "
            "then wait for the final word of this long reason: LASTWORD";
        QVERIFY(Put("bg.exit", "1"));
        QVERIFY(Put("bg.stderr", reason.toUtf8() + "\n"));
        FillTheDraft();
        // The panel at its minimum width.
        QCOMPARE(PanelOf(LIST)->property("minimumWidth").toInt(), 200);
        Window()->resize(Window()->width() / 6, Window()->height());
        QTRY_COMPARE_WITH_TIMEOUT(PanelOf(LIST)->width(), qreal(200),
                                  TIMEOUT_MS);

        VerifyFailureKeepsTheDraft("LASTWORD");

        const QQuickItem* const problem = Part("sessionsNewProblem");
        QVERIFY(problem->property("lineCount").toInt() > 1);
        QVERIFY(problem->property("contentWidth").toReal() <=
                problem->width() + 1);
    }

    // In a low panel the form scrolls; Create from the keyboard moves the
    // keyboard (and the view) to the task at the top, and the reason under
    // Create is still brought into view.
    void TheReasonIsScrolledIntoViewInALowPanel() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("bg.exit", "1"));
        QVERIFY(Put("bg.stderr", "Workspace not trusted: LASTWORD\n"));
        FillTheDraft();
        Window()->resize(Window()->width(), 260);
        QQuickItem* const scroll = Part("sessionsNewScroll");
        QTRY_VERIFY_WITH_TIMEOUT(
            scroll->property("contentHeight").toReal() > scroll->height() + 40,
            TIMEOUT_MS);
        CreateButton()->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY_WITH_TIMEOUT(CreateButton()->hasActiveFocus(), TIMEOUT_MS);
        Press(Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(Problem().contains("LASTWORD"), TIMEOUT_MS);
        const QQuickItem* const problem = Part("sessionsNewProblem");
        QTRY_VERIFY_WITH_TIMEOUT(
            problem->mapToItem(scroll, QPointF(0, 0)).y() >= 0 &&
                problem->mapToItem(scroll, QPointF(0, problem->height())).y() <=
                    scroll->height(),
            TIMEOUT_MS);
    }

    void ALostConnectionToRoostdSaysTheSessionMayHaveStarted() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("bg.hold", ""));
        FillTheDraft();
        Click(CreateButton());
        QTRY_COMPARE_WITH_TIMEOUT(Runs(), 1, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!CreateButton()->property("enabled").toBool(),
                                 TIMEOUT_MS);

        StopDaemon();
        // The held start is not the fake's to finish any more.
        Release();

        QTRY_VERIFY_WITH_TIMEOUT(Problem().startsWith("May have started: "),
                                 TIMEOUT_MS);
        QVERIFY(FormOpen());
        QCOMPARE(TextOf("sessionsNewTask"), QString("Ship it"));
        QCOMPARE(TextOf("sessionsNewName"), QString("Shipped"));
        QCOMPARE(TextOf("sessionsNewCategoryName"), QString("Brand new"));
        QTRY_VERIFY_WITH_TIMEOUT(CreateButton()->property("enabled").toBool(),
                                 TIMEOUT_MS);
        QVERIFY(Notice().isEmpty());
    }

    void ALateAnswerOfACancelledStartDoesNotTouchTheFormOpenedAgain() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("bg.exit", "1"));
        QVERIFY(Put("bg.stderr", "Workspace not trusted. Run it by hand.\n"));
        QVERIFY(Put("bg.hold", ""));
        FillTheDraft();
        Click(CreateButton());
        QTRY_COMPARE_WITH_TIMEOUT(Runs(), 1, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!CreateButton()->property("enabled").toBool(),
                                 TIMEOUT_MS);
        Click(BackLink());
        VerifyListShown();
        OPEN_FORM();
        Type("Second");

        Release();

        // The old start failed: its reason is for the footer, not the form.
        QTRY_VERIFY_WITH_TIMEOUT(Notice().startsWith("Not started: "),
                                 TIMEOUT_MS);
        WAIT_FOR_EFFECTS();
        QVERIFY(FormOpen());
        QCOMPARE(TextOf("sessionsNewTask"), QString("Second"));
        QVERIFY(Problem().isEmpty());
        QVERIFY(CreateButton()->property("enabled").toBool());
        QCOMPARE(TextOf("sessionsNewCreate"), QString("Create session"));
        QCOMPARE(Runs(), 1);
    }

    void NStillStartsASessionFromThePrompt() {
        FailOnWarnings();
        Start({}, false);
        if (QTest::currentTestFailed()) {
            return;
        }
        ArmStart();

        Press(Qt::Key_N);
        QTRY_VERIFY_WITH_TIMEOUT(Part("sessionsPromptInput")->hasActiveFocus(),
                                 TIMEOUT_MS);
        Type("do it");
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(TextOf("sessionsPromptLabel"),
                                  QString("Category:"), TIMEOUT_MS);
        Press(Qt::Key_Return);

        QTRY_COMPARE_WITH_TIMEOUT(BgRan().value(1),
                                  QString("[--bg][--][do it]"), TIMEOUT_MS);
        QVERIFY(!FormOpen());
        QTRY_VERIFY_WITH_TIMEOUT(Ids().contains(STARTED), TIMEOUT_MS);
    }

    void TheAgentRowListsTheAgentsThatCanStartInTheOrderOfThePlugins() {
        FailOnWarnings();
        QVERIFY(AddOtherAgent());
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        OPEN_FORM();
        QVERIFY(AgentBox() != nullptr);

        // codex is an agent too, but has no `newSession`.
        QTRY_COMPARE_WITH_TIMEOUT(AgentChoices(),
                                  (QStringList{"claude", "other"}), TIMEOUT_MS);
        QCOMPARE(CurrentAgent(), QString("claude"));
        QVERIFY(NameField()->y() < AgentBox()->y());
        QVERIFY(AgentBox()->y() < CategoryBox()->y());
        Snap("1-agent-row");
    }

    void TheChosenAgentStartsTheSession() {
        FailOnWarnings();
        QVERIFY(AddOtherAgent());
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        ArmStart();
        OPEN_FORM();
        Type("Ship it");
        TO_NAME();
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(AgentBox()->hasActiveFocus(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(AgentChoices().size(), 2, TIMEOUT_MS);

        // Space opens the list, Down walks it, Enter takes the entry.
        PickWithKeys(AgentBox(), 1);
        QCOMPARE(CurrentAgent(), QString("other"));
        Click(CreateButton());

        VerifyListShown();
        QTRY_COMPARE_WITH_TIMEOUT(OtherRuns().size(), 1, TIMEOUT_MS);
        QCOMPARE(OtherRuns().value(0).value("agent").toString(),
                 QString("other"));
        QCOMPARE(OtherRuns()
                     .value(0)
                     .value("args")
                     .toObject()
                     .value("prompt")
                     .toString(),
                 QString("Ship it"));
        QCOMPARE(Runs(), 0);

        // The cursor is on a session of claude: it decides, not the choice
        // of the last form.
        OPEN_FORM();
        Type("And again");
        QTRY_COMPARE_WITH_TIMEOUT(CurrentAgent(), QString("claude"),
                                  TIMEOUT_MS);
        Click(CreateButton());

        VerifyListShown();
        QTRY_COMPARE_WITH_TIMEOUT(Runs(), 1, TIMEOUT_MS);
        QCOMPARE(OtherRuns().size(), 1);
    }

    void TheDefaultAgentFollowsTheSessionUnderTheCursor() {
        FailOnWarnings();
        QVERIFY(AddOtherAgent(EOtherBackend::Lists, {OtherSession()}));
        Start({}, true, 5);
        if (QTest::currentTestFailed()) {
            return;
        }
        for (int step = 0; step < 12 && CursorAgent() != "other"; ++step) {
            Press(Qt::Key_Down);
        }
        QCOMPARE(CursorAgent(), QString("other"));
        OPEN_FORM();
        QTRY_COMPARE_WITH_TIMEOUT(CurrentAgent(), QString("other"), TIMEOUT_MS);

        Press(Qt::Key_Escape);
        VerifyListShown();
        for (int step = 0; step < 12 && CursorAgent() != "claude"; ++step) {
            Press(Qt::Key_Up);
        }
        QCOMPARE(CursorAgent(), QString("claude"));
        OPEN_FORM();
        QTRY_COMPARE_WITH_TIMEOUT(CurrentAgent(), QString("claude"),
                                  TIMEOUT_MS);
    }

    void WithoutASessionUnderTheCursorTheLastChosenAgentIsTheDefault() {
        FailOnWarnings();
        QVERIFY(AddOtherAgent());
        Start({}, true, 0, FORM_CLAUDE, true);
        if (QTest::currentTestFailed()) {
            return;
        }
        OPEN_FORM();
        QTRY_COMPARE_WITH_TIMEOUT(AgentChoices().size(), 2, TIMEOUT_MS);
        // None chosen yet: the first.
        QCOMPARE(CurrentAgent(), QString("claude"));
        Tab();
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(AgentBox()->hasActiveFocus(), TIMEOUT_MS);
        PickWithKeys(AgentBox(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(StoredNewAgent(), QString("other"),
                                  TIMEOUT_MS);

        // Nothing was created: the choice is kept all the same.
        Press(Qt::Key_Escape);
        VerifyListShown();
        OPEN_FORM();
        QTRY_COMPARE_WITH_TIMEOUT(CurrentAgent(), QString("other"), TIMEOUT_MS);
        Press(Qt::Key_Escape);
        VerifyListShown();

        // And in a new window: it is kept in the view of the panel.
        CloseWindow();
        OpenWindow();
        WaitForTheListToBeReady();
        if (QTest::currentTestFailed()) {
            return;
        }
        OPEN_FORM();
        QTRY_COMPARE_WITH_TIMEOUT(CurrentAgent(), QString("other"), TIMEOUT_MS);
    }

    void AnAgentWhoseListFailsIsMarkedAndRefused() {
        FailOnWarnings();
        QVERIFY(AddOtherAgent(EOtherBackend::FailsToList));
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        VerifyUnavailableAgentIsMarkedAndRefused("other is broken");
    }

    void AnAgentWithoutItsAdapterBinaryIsMarkedAndRefused() {
        FailOnWarnings();
        QVERIFY(AddOtherAgent(EOtherBackend::NoBinary));
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        VerifyUnavailableAgentIsMarkedAndRefused(
            "the backend of other is not running");
    }

    void AnAgentWhoseBackendIsNotRunningIsMarkedAndRefused() {
        FailOnWarnings();
        QVERIFY(AddOtherAgent(EOtherBackend::ExitsAtStart));
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        VerifyUnavailableAgentIsMarkedAndRefused(
            "the backend of other is not running");
    }

    void TheAgentOfTheCursorStaysChosenWhileAnotherListFailsAndRecovers() {
        FailOnWarnings();
        QVERIFY(AddOtherAgent(EOtherBackend::Lists, {OtherSession()}));
        Start({}, true, 5);
        if (QTest::currentTestFailed()) {
            return;
        }
        VerifyChosenAgentStaysWhenAnotherListFailsAndRecovers(false);
    }

    void ThePickedAgentStaysChosenWhileAnotherListFailsAndRecovers() {
        FailOnWarnings();
        QVERIFY(AddOtherAgent());
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        VerifyChosenAgentStaysWhenAnotherListFailsAndRecovers(true);
    }

    void TheAgentOfTheCursorBecomesUnavailableAndRecovers() {
        FailOnWarnings();
        QVERIFY(AddOtherAgent(EOtherBackend::Lists, {OtherSession()}));
        Start({}, true, 5);
        if (QTest::currentTestFailed()) {
            return;
        }
        VerifyChosenAgentBecomesUnavailableAndRecovers(false);
    }

    void ThePickedAgentBecomesUnavailableAndRecovers() {
        FailOnWarnings();
        QVERIFY(AddOtherAgent());
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        VerifyChosenAgentBecomesUnavailableAndRecovers(true);
    }

    void CreateGivesTheKeyboardToTheAgentRowWhenTheChosenAgentFails() {
        FailOnWarnings();
        QVERIFY(AddOtherAgent());
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenFormWithOtherChosen(true);
        if (QTest::currentTestFailed()) {
            return;
        }
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(CategoryBox()->hasActiveFocus(), TIMEOUT_MS);
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(CreateButton()->hasActiveFocus(), TIMEOUT_MS);

        QVERIFY(SetOtherList(true));
        ListAgainNow();

        // Create goes off with the keyboard on it: the agent row takes it.
        QTRY_VERIFY_WITH_TIMEOUT(!CreateButton()->property("enabled").toBool(),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(AgentBox()->hasActiveFocus(), TIMEOUT_MS);
        Press(Qt::Key_Escape);
        VerifyListShown();
    }

    void AListOfTheAgentsThatFailsKeepsCreateOffUntilRoostdAnswers() {
        FailOnWarnings();
        QVERIFY(WriteFakeClaude(FORM_CLAUDE));
        // A panel whose API is not connected yet: plugins.list fails.
        SelectStyleOnce();
        api::Api api;
        QQmlEngine engine;
        auto* singleton =
            engine.singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        QVERIFY(singleton != nullptr);
        singleton->setApi(api);
        engine.addImportPath(QStringLiteral(ROOST_PLUGINS_DIR "/sessions/qml"));
        QQmlComponent component(
            &engine, QUrl::fromLocalFile(QStringLiteral(
                         ROOST_PLUGINS_DIR "/sessions/qml/SessionsPanel.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        const std::unique_ptr<QObject> panel(component.create());
        QVERIFY(panel != nullptr);
        const auto part = [&](const char* name) {
            return panel->findChild<QQuickItem*>(name);
        };
        const auto problem = [&] {
            const QQuickItem* const label = part("sessionsNewProblem");
            return label->isVisible() ? label->property("text").toString()
                                      : QString();
        };
        const auto canCreate = [&] {
            return part("sessionsNewCreate")->property("enabled").toBool();
        };
        const auto agents = [&] {
            return part("sessionsNewAgent")->property("model").toStringList();
        };

        QVERIFY(QMetaObject::invokeMethod(panel.get(), "openNewForm"));
        QVERIFY(panel->setProperty("draftTask", "Ship it"));

        // Not "No agent can start a session": no list of agents came.
        QTRY_VERIFY2_WITH_TIMEOUT(problem().startsWith("No agent list: "),
                                  qPrintable(problem()), TIMEOUT_MS);
        QCOMPARE(part("sessionsNewAgent")->property("displayText").toString(),
                 QString("No agent list"));
        QVERIFY(!canCreate());
        QVERIFY(QMetaObject::invokeMethod(panel.get(), "createFromForm"));
        QVERIFY(!panel->property("newStarting").toBool());

        // The row asks again when it opens (no window here to open its
        // popup: the signal of the popup is what the form listens to).
        const int asked = panel->property("newAgentsAsked").toInt();
        QVERIFY(QMetaObject::invokeMethod(PopupOf(part("sessionsNewAgent")),
                                          "aboutToShow"));
        QVERIFY(panel->property("newAgentsAsked").toInt() > asked);
        QVERIFY(!canCreate());

        // roostd comes: the form asks again by itself.
        ArmStart();
        QVERIFY(StartDaemon());
        api.Connect(SocketPath());
        QTRY_COMPARE_WITH_TIMEOUT(agents(), (QStringList{"claude"}), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(problem().isEmpty(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(canCreate(), TIMEOUT_MS);
        QVERIFY(QMetaObject::invokeMethod(panel.get(), "createFromForm"));
        QTRY_COMPARE_WITH_TIMEOUT(Runs(), 1, TIMEOUT_MS);
    }

    void AnUnavailableDefaultAgentIsRefusedAndAnotherOneStarts() {
        FailOnWarnings();
        QVERIFY(AddOtherAgent());
        // `claude agents` fails: claude is the first agent, so the default.
        Start({}, true, 0, BROKEN_CLAUDE);
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(UnavailableNow().contains("claude is broken"),
                                 TIMEOUT_MS);
        OPEN_FORM();
        Type("Ship it");
        QTRY_COMPARE_WITH_TIMEOUT(
            AgentChoices(), (QStringList{"claude (unavailable)", "other"}),
            TIMEOUT_MS);
        QCOMPARE(CurrentAgent(), QString("claude (unavailable)"));
        QTRY_VERIFY_WITH_TIMEOUT(Problem().contains("claude is broken"),
                                 TIMEOUT_MS);
        QVERIFY(!CreateButton()->property("enabled").toBool());

        TO_NAME();
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(AgentBox()->hasActiveFocus(), TIMEOUT_MS);
        PickWithKeys(AgentBox(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(Problem().isEmpty(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(CreateButton()->property("enabled").toBool(),
                                 TIMEOUT_MS);
        Click(CreateButton());
        VerifyListShown();
        QTRY_COMPARE_WITH_TIMEOUT(OtherRuns().size(), 1, TIMEOUT_MS);
        QCOMPARE(Runs(), 0);
    }

    void
    AChosenAgentThatCannotStartAnyMoreFailsWithTheReasonAndKeepsTheDraft() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        FillTheDraft();
        QTRY_COMPARE_WITH_TIMEOUT(AgentChoices(), (QStringList{"claude"}),
                                  TIMEOUT_MS);
        // The list of the form is older than the plugins: the chosen agent is
        // gone from roostd's list of those that can start.
        QVERIFY(PanelOf(LIST)->setProperty("draftAgent", "ghost"));

        VerifyFailureKeepsTheDraft("ghost cannot start a session");
        QCOMPARE(Runs(), 0);
    }

    void TheNPromptKeepsItsDefaultAgentWhateverTheFormChose() {
        FailOnWarnings();
        QVERIFY(AddOtherAgent());
        // No session of claude: the cursor has no session's agent, so `n`
        // would take the form's last choice if it read it.
        Start({}, true, 0, FORM_CLAUDE, true);
        if (QTest::currentTestFailed()) {
            return;
        }
        ArmStart();
        OPEN_FORM();
        QTRY_COMPARE_WITH_TIMEOUT(AgentChoices().size(), 2, TIMEOUT_MS);
        Tab();
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(AgentBox()->hasActiveFocus(), TIMEOUT_MS);
        PickWithKeys(AgentBox(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(StoredNewAgent(), QString("other"),
                                  TIMEOUT_MS);
        Press(Qt::Key_Escape);
        VerifyListShown();
        QVERIFY(CursorAgent().isEmpty());

        Press(Qt::Key_N);
        QTRY_VERIFY_WITH_TIMEOUT(Part("sessionsPromptInput")->hasActiveFocus(),
                                 TIMEOUT_MS);
        Type("do it");
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(TextOf("sessionsPromptLabel"),
                                  QString("Category:"), TIMEOUT_MS);
        Press(Qt::Key_Return);

        // The first agent (claude), as before the row, not the form's other.
        QTRY_COMPARE_WITH_TIMEOUT(Runs(), 1, TIMEOUT_MS);
        QVERIFY(OtherRuns().isEmpty());
    }
};

QTEST_MAIN(SessionsFormTest)
#include "sessions_form_test.moc"
