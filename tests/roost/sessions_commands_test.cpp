// The commands of the sessions panel in the shell's window: d, x, r, R and n
// with their confirmations, notices and prompt, and the reopen of a respawned
// session in the terminal. The window, the daemon and the fake `claude` are
// helpers/sessions_window.hpp with a fake `claude` of its own that records what
// it is asked to do; the panels, the model and the plugin are the real ones.
// HOME is a temporary directory, and no real `claude` runs.

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlExtensionPlugin>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTest>

#include <signal.h>

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

// The keys of the sessions in the document `sessions`.
QString KeyOf(const QString& id) { return "claude/" + id; }

// The session that `claude --bg` starts: not in the fixture.
const QString STARTED = "bg-4e5f6071";

// A fake `claude`, steered by the files next to it.
//   agents    prints agents.json with the cwds of /work moved into this
//             directory, which it makes
//   attach    adds the id to calls, leaves its pid in attach-<id>.pid, and
//             runs fake_tui
//   stop, rm, respawn
//             add "<verb> <id>" to commands; fail with the exit code in
//             <verb>.exit and "No job matching" when that file is there;
//             otherwise end the attach of the id (all of them for --all), as
//             the real claude does, unless <verb>.nokill is there; wait
//             <verb>.sleep seconds after that. `rm` puts rm.agents in place of
//             agents.json when it is there.
//   --bg      writes the directory it runs in and its arguments, each whole in
//             brackets, to bg.ran; fails with bg.exit and the text of
//             bg.stderr, or puts bg.agents in place of agents.json and prints
//             bg.stdout
constexpr const char* COMMANDS_CLAUDE = R"sh(#!/bin/sh
dir=$(dirname "$0")
verb=$1
argv=; for a in "$@"; do argv="$argv[$a]"; done
if [ "$verb" = --bg ]; then verb=bg; fi
case "$verb" in
agents)
  mkdir -p "$dir/work/alpha" "$dir/work/beta" "$dir/work/gamma" "$dir/work/delta" "$dir/work/other"
  sed 's|"/work/|"'"$dir"'/work/|g' "$dir/agents.json"
  ;;
attach)
  echo "$2" >> "$dir/calls"
  echo $$ > "$dir/attach-$2.pid"
  cd / && FAKE_TUI_GREETING=$2 exec "$dir/fake_tui"
  ;;
stop|rm|respawn)
  echo "$1 $2" >> "$dir/commands"
  if [ -f "$dir/$1.exit" ]; then
    echo "No job matching '$2'" >&2
    exit $(cat "$dir/$1.exit")
  fi
  if [ ! -f "$dir/$1.nokill" ]; then
    if [ "$2" = --all ]; then pids="$dir"/attach-*.pid; else pids="$dir/attach-$2.pid"; fi
    for pid in $pids; do
      if [ -f "$pid" ]; then kill $(cat "$pid") 2>/dev/null; rm -f "$pid"; fi
    done
  fi
  if [ "$1" = rm ] && [ -f "$dir/rm.agents" ]; then
    mv "$dir/rm.agents" "$dir/agents.json"
  fi
  if [ -f "$dir/$1.sleep" ]; then sleep "$(cat "$dir/$1.sleep")"; fi
  ;;
bg)
  printf '%s\n%s\n' "$(pwd -P)" "$argv" > "$dir/bg.ran"
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

class SessionsCommandsTest : public SessionsWindow {
    Q_OBJECT

private:
    std::optional<ScopedEnv> Home_;
    QString TabNowOfList() { return List()->property("tab").toString(); }

    int SessionsCount() { return Singleton_->sessions()->rowCount(); }

    QString BinPath(const QString& name) const {
        return Dir_->filePath("bin/" + name);
    }

    bool Put(const QString& name, const QByteArray& content) const {
        QSaveFile file(BinPath(name));
        return file.open(QIODevice::WriteOnly) && file.write(content) >= 0 &&
               file.commit();
    }

    // The lines of a file the fake claude appends to.
    QStringList Lines(const QString& name) const {
        return QString::fromUtf8(ReadFile(BinPath(name)))
            .split('\n', Qt::SkipEmptyParts);
    }

    QStringList Commands() const { return Lines("commands"); }
    QStringList Calls() const { return Lines("calls"); }

    // The fixture's sessions but `gone`, as JSON for a file of the fake.
    static QByteArray AgentsWithout(const QStringList& gone,
                                    const QJsonObject& added = {}) {
        QJsonArray left;
        for (const QJsonValue& entry :
             QJsonDocument::fromJson(
                 ReadFile(QStringLiteral(FIXTURES_DIR "/claude_agents.json")))
                 .array()) {
            if (!gone.contains(entry.toObject().value("id").toString())) {
                left.append(entry);
            }
        }
        if (!added.isEmpty()) {
            left.append(added);
        }
        return QJsonDocument(left).toJson(QJsonDocument::Compact);
    }

    // What `claude agents` lists once STARTED is there.
    static QByteArray AgentsWithStarted() {
        return AgentsWithout({},
                             QJsonObject{{"id", STARTED},
                                         {"cwd", "/work/beta"},
                                         {"kind", "background"},
                                         {"startedAt", qint64{1790150400000}},
                                         {"name", "Do it"},
                                         {"state", "working"},
                                         {"status", "busy"}});
    }

    // The document `sessions` as the daemon finds it at the start.
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

    void Start(const QJsonObject& sections = {}, int rows = 4) {
        if (!sections.isEmpty()) {
            QVERIFY(Seed(sections));
        }
        Boot(rows, COMMANDS_CLAUDE);
    }

    QQuickItem* Part(const QString& name) {
        return PanelOf(LIST)->findChild<QQuickItem*>(name);
    }

    bool Shown(const QString& name) { return Part(name)->isVisible(); }
    QString TextOf(const QString& name) {
        return Part(name)->property("text").toString();
    }

    QString Confirm() {
        return Shown("sessionsConfirm") ? TextOf("sessionsConfirm") : QString();
    }
    QString Notice() {
        return Shown("sessionsNotice") ? TextOf("sessionsNotice") : QString();
    }
    bool Prompting() { return Shown("sessionsPrompt"); }
    QString PromptLabel() { return TextOf("sessionsPromptLabel"); }
    QString PromptText() { return TextOf("sessionsPromptInput"); }

    int Unread() {
        return List()->property("counts").toMap().value("unread").toInt();
    }

    void MoveTo(const QString& id) {
        for (int step = 0; step < 8 && CursorId() != id; ++step) {
            Press(Qt::Key_Down);
        }
        QCOMPARE(CursorId(), id);
    }

    // d, x or r, then y; waits for the question to be on screen first.
    void AskAndConfirm(Qt::Key key, Qt::KeyboardModifiers modifiers = {}) {
        Press(key, modifiers);
        QTRY_VERIFY_WITH_TIMEOUT(!Confirm().isEmpty(), TIMEOUT_MS);
        Press(Qt::Key_Y);
        QTRY_VERIFY_WITH_TIMEOUT(Confirm().isEmpty(), TIMEOUT_MS);
    }

    // n with the task and the category as typed, ending with the Enters.
    void StartNewSession(const QString& task, const QString& category = {}) {
        Press(Qt::Key_N);
        QTRY_VERIFY_WITH_TIMEOUT(Prompting(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Part("sessionsPromptInput")->hasActiveFocus(),
                                 TIMEOUT_MS);
        QCOMPARE(PromptLabel(), QString("Task:"));
        Type(task);
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(PromptLabel(), QString("Category:"),
                                  TIMEOUT_MS);
        if (!category.isEmpty()) {
            Type(category);
        }
        Press(Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!Prompting(), TIMEOUT_MS);
    }

    // Typed text: a press of each character, to whatever has the focus.
    void Type(const QString& text) {
        for (const QChar c : text) {
            QTest::sendKeyEvent(QTest::Click, Window(),
                                static_cast<Qt::Key>(c.toUpper().unicode()),
                                QString(c), Qt::NoModifier);
        }
    }

    // The cwd and the arguments the fake claude was started with by --bg.
    QStringList BgRan() const { return Lines("bg.ran"); }

    static QString Canonical(const QString& path) {
        return QFileInfo(path).canonicalFilePath();
    }

    // The session in the terminal, open and settled; the keyboard back in the
    // list.
    void OpenAndBack(int session) {
        Watch();
        OpenRow(session);
        BackToList();
    }

    // The pid the fake claude left for the attach of the session.
    qint64 AttachPid(const QString& id) const {
        return ReadFile(BinPath("attach-" + id + ".pid"))
            .trimmed()
            .toLongLong();
    }

    // A document of the stored `sessions` holds nothing of the session.
    bool StoredHas(const QString& id) {
        return QJsonDocument(StoredDocument())
            .toJson()
            .contains(KeyOf(id).toUtf8());
    }

private slots:
    void init() {
        Prepare();
        QVERIFY(QDir().mkpath(Dir_->filePath("home")));
        Home_.emplace("HOME", Dir_->filePath("home").toUtf8());
    }

    void cleanup() {
        Teardown();
        Home_.reset();
    }

    void DAsksAndAnotherKeyCancels() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }

        Press(Qt::Key_D);
        QTRY_VERIFY_WITH_TIMEOUT(!Confirm().isEmpty(), TIMEOUT_MS);
        QVERIFY(Confirm().contains("Fix the flaky test"));
        QVERIFY(Confirm().contains("Delete"));

        // j cancels and is taken: the cursor stays.
        Press(Qt::Key_J);
        QTRY_VERIFY_WITH_TIMEOUT(Confirm().isEmpty(), TIMEOUT_MS);
        QCOMPARE(CursorId(), ALPHA);

        // Enter cancels and opens nothing; so does Esc.
        Press(Qt::Key_D);
        QTRY_VERIFY_WITH_TIMEOUT(!Confirm().isEmpty(), TIMEOUT_MS);
        Press(Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(Confirm().isEmpty(), TIMEOUT_MS);
        QVERIFY(SelectedId().isEmpty());
        Press(Qt::Key_D);
        QTRY_VERIFY_WITH_TIMEOUT(!Confirm().isEmpty(), TIMEOUT_MS);
        Press(Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(Confirm().isEmpty(), TIMEOUT_MS);

        // Nothing ran: the one command after them is the only one.
        AskAndConfirm(Qt::Key_R);
        QTRY_COMPARE_WITH_TIMEOUT(Commands(), QStringList{"respawn " + ALPHA},
                                  TIMEOUT_MS);
    }

    void DThenYDeletesAndForgetsAfterRemoval() {
        FailOnWarnings();
        Start(QJsonObject{{"names", QJsonObject{{KeyOf(GAMMA), "Cleanup"}}},
                          {"categories", QJsonObject{{KeyOf(GAMMA), "Ops"}}}});
        if (QTest::currentTestFailed()) {
            return;
        }
        // The list claude prints right after rm still has the session: the
        // adapter reports a removal with a later list, which the case sends.
        QVERIFY(Put("rm.agents", AgentsWithout({})));
        MoveTo(GAMMA);

        Press(Qt::Key_D);
        QTRY_VERIFY_WITH_TIMEOUT(!Confirm().isEmpty(), TIMEOUT_MS);
        QVERIFY(Confirm().contains("Cleanup"));
        Press(Qt::Key_Y);

        QTRY_COMPARE_WITH_TIMEOUT(Commands(), QStringList{"rm " + GAMMA},
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Notice().contains("deleted"), TIMEOUT_MS);
        // Answered, but still listed: the list keeps what it knows of it, or
        // it would take the session for a new one.
        QTest::qWait(500);
        QVERIFY(Ids().contains(GAMMA));
        QVERIFY(StoredHas(GAMMA));
        QCOMPARE(Role(ViewRow(static_cast<int>(Ids().indexOf(GAMMA))), "title")
                     .toString(),
                 QString("Cleanup"));

        QVERIFY(Put("agents.json", AgentsWithout({GAMMA})));

        QTRY_COMPARE_WITH_TIMEOUT(Ids(), (QStringList{ALPHA, BETA, DELTA}),
                                  TIMEOUT_MS);
        // Gone from Roost.sessions: the document is cleaned.
        QTRY_VERIFY_WITH_TIMEOUT(!StoredHas(GAMMA), TIMEOUT_MS);
        QCOMPARE(Unread(), 0);
    }

    void RussianDThenNDeletes() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("rm.agents", AgentsWithout({ALPHA})));

        // "в" on the D key (kVK_ANSI_D 0x02, KEY_D + 8), "н" on the Y key
        // (kVK_ANSI_Y 0x10).
        PressNative(0x0412, QStringLiteral("в"), 0x02, 40);
        QTRY_VERIFY_WITH_TIMEOUT(!Confirm().isEmpty(), TIMEOUT_MS);
        PressNative(0x041D, QStringLiteral("н"), 0x10, 29);

        QTRY_COMPARE_WITH_TIMEOUT(Commands(), QStringList{"rm " + ALPHA},
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Ids(), (QStringList{BETA, GAMMA, DELTA}),
                                  TIMEOUT_MS);
    }

    void AFailedDeleteKeepsTheSession() {
        FailOnWarnings();
        Start(QJsonObject{{"names", QJsonObject{{KeyOf(ALPHA), "Mine"}}},
                          {"categories", QJsonObject{{KeyOf(ALPHA), "Ops"}}}});
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("rm.exit", "1"));

        AskAndConfirm(Qt::Key_D);

        QTRY_VERIFY_WITH_TIMEOUT(Notice().startsWith("Not deleted: "),
                                 TIMEOUT_MS);
        QVERIFY(Notice().contains("No job matching"));
        QCOMPARE(Commands(), QStringList{"rm " + ALPHA});
        QCOMPARE(Count(), 4);
        QVERIFY(StoredHas(ALPHA));
        QCOMPARE(Role(1, "title").toString(), QString("Mine"));
    }

    void DOnTheOpenSessionClosesItAtOnce() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("rm.agents", AgentsWithout({ALPHA})));
        OpenAndBack(0);
        QCOMPARE(SelectedId(), ALPHA);

        Press(Qt::Key_D);
        QTRY_VERIFY_WITH_TIMEOUT(!Confirm().isEmpty(), TIMEOUT_MS);
        Press(Qt::Key_Y);

        // Before the command is answered, the window does not hold it.
        QVERIFY(SelectedId().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("none"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Ids(), (QStringList{BETA, GAMMA, DELTA}),
                                  TIMEOUT_MS);
        // Not opened again by anything.
        QTRY_COMPARE_WITH_TIMEOUT(Calls(), QStringList{ALPHA}, TIMEOUT_MS);
    }

    void XStopsAndArchivesAndClosesTheOpenSession() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenAndBack(0);

        Press(Qt::Key_X);
        QTRY_VERIFY_WITH_TIMEOUT(Confirm().contains("Fix the flaky test"),
                                 TIMEOUT_MS);
        Press(Qt::Key_Y);

        QTRY_COMPARE_WITH_TIMEOUT(Commands(), QStringList{"stop " + ALPHA},
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Ids(), (QStringList{BETA, GAMMA, DELTA}),
                                  TIMEOUT_MS);
        QVERIFY(SelectedId().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("none"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(StoredDocument().value("archived").toArray(),
                                  QJsonArray{KeyOf(ALPHA)}, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Notice().contains("archived"), TIMEOUT_MS);
        // The session is still listed: archived, not deleted.
        QCOMPARE(SessionsCount(), 4);
    }

    void AFailedStopLeavesItArchivedWithANotice() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("stop.exit", "1"));

        AskAndConfirm(Qt::Key_X);

        QTRY_VERIFY_WITH_TIMEOUT(Notice().startsWith("Not stopped: "),
                                 TIMEOUT_MS);
        QVERIFY(Notice().contains("No job matching"));
        QTRY_COMPARE_WITH_TIMEOUT(StoredDocument().value("archived").toArray(),
                                  QJsonArray{KeyOf(ALPHA)}, TIMEOUT_MS);
        QCOMPARE(Ids(), (QStringList{BETA, GAMMA, DELTA}));
    }

    void XInTheArchiveRestoresWithoutAsking() {
        FailOnWarnings();
        Start(QJsonObject{{"archived", QJsonArray{KeyOf(GAMMA)}}}, 3);
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_V);
        QTRY_COMPARE_WITH_TIMEOUT(Ids(), (QStringList{GAMMA}), TIMEOUT_MS);

        Press(Qt::Key_X);

        QTRY_COMPARE_WITH_TIMEOUT(Ids(), QStringList{}, TIMEOUT_MS);
        QVERIFY(Confirm().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(StoredDocument().value("archived").toArray(),
                                  QJsonArray{}, TIMEOUT_MS);
        Press(Qt::Key_V);
        QTRY_COMPARE_WITH_TIMEOUT(Ids(), ORDER, TIMEOUT_MS);
        // No command ran: the one after is the only one.
        MoveTo(GAMMA);
        AskAndConfirm(Qt::Key_R);
        QTRY_COMPARE_WITH_TIMEOUT(Commands(), QStringList{"respawn " + GAMMA},
                                  TIMEOUT_MS);
    }

    void RAsksThenRespawns() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QSignalSpy respawned(PanelOf(LIST), SIGNAL(respawned(QVariant)));
        MoveTo(BETA);

        Press(Qt::Key_R);
        QTRY_VERIFY_WITH_TIMEOUT(Confirm().contains("Review the PR"),
                                 TIMEOUT_MS);
        QVERIFY(Confirm().contains("Respawn"));
        Press(Qt::Key_Y);

        QTRY_COMPARE_WITH_TIMEOUT(Commands(), QStringList{"respawn " + BETA},
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Notice().contains("Review the PR") &&
                                     Notice().contains("respawned"),
                                 TIMEOUT_MS);
        // It is not the open session, so the terminal is not asked.
        QCOMPARE(respawned.count(), 0);
    }

    void ShiftRCountsWorkingAndRespawnsAll() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }

        Press(Qt::Key_R, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(!Confirm().isEmpty(), TIMEOUT_MS);
        QVERIFY2(Confirm().contains("(2 running)"), qPrintable(Confirm()));
        Press(Qt::Key_Y);

        QTRY_COMPARE_WITH_TIMEOUT(Commands(), QStringList{"respawn --all"},
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Notice(), QString("Respawned all"),
                                  TIMEOUT_MS);
    }

    // The old attach ends, as the real claude ends it, and the window opens
    // the session again by itself: a new attach, with no key.
    void RespawnOfTheOpenSessionReopensIt() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QSignalSpy respawned(PanelOf(LIST), SIGNAL(respawned(QVariant)));
        OpenAndBack(1);
        QTRY_COMPARE_WITH_TIMEOUT(Calls(), QStringList{BETA}, TIMEOUT_MS);

        AskAndConfirm(Qt::Key_R);

        QTRY_COMPARE_WITH_TIMEOUT(Calls(), (QStringList{BETA, BETA}),
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        Settle();
        QCOMPARE(respawned.count(), 1);
        QCOMPARE(Commands(), QStringList{"respawn " + BETA});
        // The old attach ended, and the same session is shown again.
        QVERIFY(Changes_.filter(QRegularExpression("^ended\\|")).size() >= 1);
        QCOMPARE(SelectedId(), BETA);
        QVERIFY(ScreenNow().contains(Greeting(BETA)));
    }

    // The old attach has ended by the time the command is answered.
    void ReopensWhenTheAttachEndedBeforeTheAnswer() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("respawn.sleep", "1"));
        OpenAndBack(1);

        AskAndConfirm(Qt::Key_R);

        QTRY_COMPARE_WITH_TIMEOUT(Calls(), (QStringList{BETA, BETA}),
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
    }

    // The command is answered while the attach is still open; it ends later.
    void ReopensWhenTheAttachEndsAfterTheAnswer() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("respawn.nokill", ""));
        OpenAndBack(1);

        AskAndConfirm(Qt::Key_R);
        QTRY_VERIFY_WITH_TIMEOUT(Notice().contains("respawned"), TIMEOUT_MS);
        QCOMPARE(Status(), QString("open"));
        QTRY_COMPARE_WITH_TIMEOUT(Calls(), QStringList{BETA}, TIMEOUT_MS);

        QCOMPARE(::kill(static_cast<pid_t>(AttachPid(BETA)), SIGTERM), 0);

        QTRY_COMPARE_WITH_TIMEOUT(Calls(), (QStringList{BETA, BETA}),
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
    }

    void AnAttachThatEndsMuchLaterIsNotReopened() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("respawn.nokill", ""));
        OpenAndBack(1);
        QVERIFY(PanelOf(TERMINAL)->setProperty("reopenWaitMs", 100));

        AskAndConfirm(Qt::Key_R);
        QTRY_VERIFY_WITH_TIMEOUT(Notice().contains("respawned"), TIMEOUT_MS);
        QTest::qWait(400);
        QCOMPARE(::kill(static_cast<pid_t>(AttachPid(BETA)), SIGTERM), 0);

        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("ended"), TIMEOUT_MS);
        QTest::qWait(300);
        QCOMPARE(Status(), QString("ended"));
        QTRY_COMPARE_WITH_TIMEOUT(Calls(), QStringList{BETA}, TIMEOUT_MS);
    }

    void RespawnOfAnotherSessionLeavesTheOpenOneAlone() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QSignalSpy respawned(PanelOf(LIST), SIGNAL(respawned(QVariant)));
        OpenAndBack(0);
        MoveTo(BETA);

        AskAndConfirm(Qt::Key_R);

        QTRY_COMPARE_WITH_TIMEOUT(Commands(), QStringList{"respawn " + BETA},
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Notice().contains("respawned"), TIMEOUT_MS);
        QTest::qWait(300);
        QCOMPARE(respawned.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(Calls(), QStringList{ALPHA}, TIMEOUT_MS);
        QCOMPARE(Status(), QString("open"));
    }

    void RespawnAllReopensTheRunningOpenSessionOnly() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        // ALPHA is working: its attach ends and it is opened again.
        OpenAndBack(0);
        Press(Qt::Key_R, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(!Confirm().isEmpty(), TIMEOUT_MS);
        Press(Qt::Key_Y);
        QTRY_COMPARE_WITH_TIMEOUT(Calls(), (QStringList{ALPHA, ALPHA}),
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        Settle();
        QTRY_COMPARE_WITH_TIMEOUT(Notice(), QString("Respawned all"),
                                  TIMEOUT_MS);

        // GAMMA is done: respawn --all ends its attach, and it stays ended.
        BackToList();
        MoveTo(GAMMA);
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(Calls(), (QStringList{ALPHA, ALPHA, GAMMA}),
                                  TIMEOUT_MS);
        BackToList();
        Press(Qt::Key_R, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(!Confirm().isEmpty(), TIMEOUT_MS);
        Press(Qt::Key_Y);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("ended"), TIMEOUT_MS);
        QTest::qWait(300);
        QCOMPARE(Status(), QString("ended"));
        QTRY_COMPARE_WITH_TIMEOUT(Calls(), (QStringList{ALPHA, ALPHA, GAMMA}),
                                  TIMEOUT_MS);
    }

    void NStartsASessionInTheCursorsDirectory() {
        FailOnWarnings();
        Start(QJsonObject{{"categories", QJsonObject{{KeyOf(BETA), "Ops"}}}});
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("bg.stdout",
                    "backgrounded · " + STARTED.toUtf8() + " · Do it\n"));
        QVERIFY(Put("bg.agents", AgentsWithStarted()));
        QSignalSpy activated(PanelOf(LIST), SIGNAL(activated()));
        Watch();
        MoveTo(BETA);

        Press(Qt::Key_N);
        QTRY_VERIFY_WITH_TIMEOUT(Prompting(), TIMEOUT_MS);
        QCOMPARE(PromptLabel(), QString("Task:"));
        Type("do it");
        QCOMPARE(PromptText(), QString("do it"));
        Press(Qt::Key_Return);
        // The category is prefilled with the cursor's.
        QTRY_COMPARE_WITH_TIMEOUT(PromptLabel(), QString("Category:"),
                                  TIMEOUT_MS);
        QCOMPARE(PromptText(), QString("Ops"));
        Press(Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!Prompting(), TIMEOUT_MS);

        QTRY_COMPARE_WITH_TIMEOUT(BgRan().size(), 2, TIMEOUT_MS);
        QCOMPARE(Canonical(BgRan().value(0)), Canonical(BinPath("work/beta")));
        QCOMPARE(BgRan().value(1), QString("[--bg][--][do it]"));
        // The session is listed with the next list of the agent, is under
        // "Ops", and is opened in the terminal.
        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), STARTED, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Status(), QString("open"), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Calls(), QStringList{STARTED}, TIMEOUT_MS);
        QVERIFY(activated.count() >= 1);
        const int row = List()->property("cursorRow").toInt();
        QCOMPARE(Role(row, "id").toString(), STARTED);
        QCOMPARE(Role(row, "category").toString(), QString("Ops"));
        QTRY_COMPARE_WITH_TIMEOUT(StoredDocument()
                                      .value("categories")
                                      .toObject()
                                      .value(KeyOf(STARTED)),
                                  QJsonValue("Ops"), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Notice().contains("Do it"), TIMEOUT_MS);
    }

    // The Category step of n is the field of c: a digit in the empty field
    // picks the known category with that number and starts the session.
    void ADigitPicksAKnownCategoryInNsCategoryStep() {
        FailOnWarnings();
        Start(QJsonObject{{"categories", QJsonObject{{KeyOf(BETA), "Ops"}}}});
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("bg.stdout",
                    "backgrounded · " + STARTED.toUtf8() + " · Do it\n"));
        QVERIFY(Put("bg.agents", AgentsWithStarted()));
        MoveTo(ALPHA);

        Press(Qt::Key_N);
        QTRY_VERIFY_WITH_TIMEOUT(Prompting(), TIMEOUT_MS);
        Type("do it");
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(PromptLabel(), QString("Category:"),
                                  TIMEOUT_MS);
        // The session under the cursor has no category: the field is empty.
        QCOMPARE(PromptText(), QString());
        Press(Qt::Key_1);
        QTRY_VERIFY_WITH_TIMEOUT(!Prompting(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(BgRan().size(), 2, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(StoredDocument()
                                      .value("categories")
                                      .toObject()
                                      .value(KeyOf(STARTED)),
                                  QJsonValue("Ops"), TIMEOUT_MS);
    }

    // A folded header: n takes its category, and there is no session, so the
    // directory is the home directory and the agent is the one that has
    // newSession.
    void NOnAHeaderTakesItsCategory() {
        FailOnWarnings();
        Start(QJsonObject{{"categories", QJsonObject{{KeyOf(BETA), "Ops"}}}});
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("bg.stdout",
                    "backgrounded · " + STARTED.toUtf8() + " · Do it\n"));
        QVERIFY(Put("bg.agents", AgentsWithStarted()));
        MoveTo(BETA);
        Press(Qt::Key_Z);
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), ON_HEADER, TIMEOUT_MS);

        Press(Qt::Key_N);
        QTRY_VERIFY_WITH_TIMEOUT(Prompting(), TIMEOUT_MS);
        Type("do it");
        Press(Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(PromptLabel(), QString("Category:"),
                                  TIMEOUT_MS);
        QCOMPARE(PromptText(), QString("Ops"));
        Press(Qt::Key_Return);

        QTRY_COMPARE_WITH_TIMEOUT(BgRan().size(), 2, TIMEOUT_MS);
        QCOMPARE(Canonical(BgRan().value(0)),
                 Canonical(Dir_->filePath("home")));
        QTRY_COMPARE_WITH_TIMEOUT(SelectedId(), STARTED, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(StoredDocument()
                                      .value("categories")
                                      .toObject()
                                      .value(KeyOf(STARTED)),
                                  QJsonValue("Ops"), TIMEOUT_MS);
    }

    void ThePromptKeepsItsTargetAcrossAPoll() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("bg.stdout",
                    "backgrounded · " + STARTED.toUtf8() + " · Do it\n"));
        MoveTo(BETA);
        Press(Qt::Key_N);
        QTRY_VERIFY_WITH_TIMEOUT(Prompting(), TIMEOUT_MS);
        Type("do it");

        // The next list has BETA in another directory.
        // (A change of the state is what the adapter reports.)
        QJsonArray changed;
        for (QJsonValue entry :
             QJsonDocument::fromJson(ReadFile(BinPath("agents.json")))
                 .array()) {
            QJsonObject object = entry.toObject();
            if (object.value("id") == BETA) {
                object.insert("cwd", "/work/other");
                object.insert("state", "done");
            }
            changed.append(object);
        }
        QVERIFY(Put("agents.json",
                    QJsonDocument(changed).toJson(QJsonDocument::Compact)));
        QTRY_VERIFY_WITH_TIMEOUT(
            Role(ViewRow(static_cast<int>(Ids().indexOf(BETA))), "cwd")
                .toString()
                .endsWith("/work/other"),
            TIMEOUT_MS);
        QVERIFY(Prompting());
        QCOMPARE(PromptText(), QString("do it"));

        Press(Qt::Key_Return);
        Press(Qt::Key_Return);

        QTRY_COMPARE_WITH_TIMEOUT(BgRan().size(), 2, TIMEOUT_MS);
        QCOMPARE(Canonical(BgRan().value(0)), Canonical(BinPath("work/beta")));
    }

    void AFailedNewSessionShowsANotice() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("bg.exit", "1"));
        QVERIFY(Put("bg.stderr", "Workspace not trusted. Run it by hand.\n"));

        StartNewSession("do it");

        QTRY_VERIFY_WITH_TIMEOUT(Notice().startsWith("Not started: "),
                                 TIMEOUT_MS);
        QVERIFY(Notice().contains("Workspace not trusted"));
        QCOMPARE(Count(), 4);
        QVERIFY(SelectedId().isEmpty());
    }

    void ThePromptTakesTextWithoutListKeysAndEscCancels() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_N);
        QTRY_VERIFY_WITH_TIMEOUT(Prompting(), TIMEOUT_MS);

        // Letters of list keys are text, and Cyrillic stays Cyrillic.
        Type("zdax");
        PressNative(0x0444, QStringLiteral("ф"), 0x00, 38);
        PressNative(0x044F, QStringLiteral("я"), 0x06, 52);
        QCOMPARE(PromptText(), QString("zdaxфя"));
        QVERIFY(Confirm().isEmpty());
        QCOMPARE(TabNowOfList(), QString("all"));
        QCOMPARE(Count(), 4);
        // An empty task starts nothing.
        Press(Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!Prompting(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);

        Press(Qt::Key_N);
        QTRY_VERIFY_WITH_TIMEOUT(Prompting(), TIMEOUT_MS);
        Press(Qt::Key_Return);
        QVERIFY(Prompting());
        QCOMPARE(PromptLabel(), QString("Task:"));
        Press(Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!Prompting(), TIMEOUT_MS);

        // Nothing was started: the next command is the only one.
        AskAndConfirm(Qt::Key_R);
        QTRY_COMPARE_WITH_TIMEOUT(Commands(), QStringList{"respawn " + ALPHA},
                                  TIMEOUT_MS);
        QVERIFY(BgRan().isEmpty());
    }

    void SessionKeysOnAHeaderDoNothing() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Z);
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), ON_HEADER, TIMEOUT_MS);

        Press(Qt::Key_D);
        Press(Qt::Key_X);
        Press(Qt::Key_R);

        QVERIFY(Confirm().isEmpty());
        // Unfolded, the next command is the only one.
        Press(Qt::Key_Z);
        QTRY_COMPARE_WITH_TIMEOUT(Count(), 4, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Cursor(), 0, TIMEOUT_MS);
        AskAndConfirm(Qt::Key_R);
        QTRY_COMPARE_WITH_TIMEOUT(Commands(), QStringList{"respawn " + ALPHA},
                                  TIMEOUT_MS);
    }

    void ConfirmIgnoresBareModifiers() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(Put("rm.agents", AgentsWithout({ALPHA})));

        Press(Qt::Key_D);
        QTRY_VERIFY_WITH_TIMEOUT(!Confirm().isEmpty(), TIMEOUT_MS);
        Press(Qt::Key_Shift);
        Press(Qt::Key_Control);
        Press(Qt::Key_Alt);
        Press(Qt::Key_Meta);
        QVERIFY(!Confirm().isEmpty());
        // A held key is not an answer either.
        PressRepeated(Qt::Key_D, Qt::NoModifier, "d");
        QVERIFY(!Confirm().isEmpty());
        Press(Qt::Key_Y);

        QTRY_COMPARE_WITH_TIMEOUT(Commands(), QStringList{"rm " + ALPHA},
                                  TIMEOUT_MS);
    }

    void YWithAModifierCancels() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_D);
        QTRY_VERIFY_WITH_TIMEOUT(!Confirm().isEmpty(), TIMEOUT_MS);

        Press(Qt::Key_Y, CONTROL_KEY);

        QTRY_VERIFY_WITH_TIMEOUT(Confirm().isEmpty(), TIMEOUT_MS);
        AskAndConfirm(Qt::Key_R);
        QTRY_COMPARE_WITH_TIMEOUT(Commands(), QStringList{"respawn " + ALPHA},
                                  TIMEOUT_MS);
    }

    void ModifiedAndHeldCommandKeysAsknothing() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }

        Press(Qt::Key_D, CONTROL_KEY);
        Press(Qt::Key_D, Qt::AltModifier);
        Press(Qt::Key_D, Qt::ShiftModifier);
        Press(Qt::Key_X, Qt::ShiftModifier);
        Press(Qt::Key_N, Qt::ShiftModifier);
        Press(Qt::Key_R, CONTROL_KEY);
        PressRepeated(Qt::Key_D, Qt::NoModifier, "d");
        PressRepeated(Qt::Key_R, Qt::ShiftModifier, "R");
        PressRepeated(Qt::Key_N, Qt::NoModifier, "n");

        QVERIFY(Confirm().isEmpty());
        QVERIFY(!Prompting());
        QCOMPARE(Count(), 4);
    }

    void TheHelpListsTheCommandKeys() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        Press(Qt::Key_Question, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(PanelOf(LIST)->property("helpOpen").toBool(),
                                 TIMEOUT_MS);

        QString help;
        for (auto* text : Part("sessionsHelp")->findChildren<QQuickItem*>()) {
            help += text->property("text").toString() + "\n";
        }
        for (const char* line : {"d   delete", "x   archive", "r   respawn",
                                 "Shift+r   respawn all", "n   new session"}) {
            QVERIFY2(help.contains(line), line);
        }
        // The help takes the command keys too.
        Press(Qt::Key_D);
        QVERIFY(Confirm().isEmpty());
    }

    void TheNoticeExpires() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(PanelOf(LIST)->setProperty("noticeMs", 300));

        AskAndConfirm(Qt::Key_R);
        QTRY_VERIFY_WITH_TIMEOUT(Notice().contains("respawned"), TIMEOUT_MS);

        QTRY_VERIFY_WITH_TIMEOUT(!Shown("sessionsNotice"), TIMEOUT_MS);
    }
};

QTEST_MAIN(SessionsCommandsTest)
#include "sessions_commands_test.moc"
