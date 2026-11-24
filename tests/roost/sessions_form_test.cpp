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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlExtensionPlugin>
#include <QSaveFile>
#include <QTest>

#include <optional>

#include "helpers/scoped_env.hpp"
#include "helpers/sessions_window.hpp"
#include "helpers/snapshot.hpp"

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

// A fake `claude`, steered by the files next to it: `agents` prints
// agents.json with the cwds of /work moved into this directory, which it
// makes; `attach` runs fake_tui; `--bg` writes the directory it runs in and
// its arguments, each whole in brackets, to bg.ran, puts bg.agents in place of
// agents.json when it is there and prints bg.stdout.
constexpr const char* FORM_CLAUDE = R"sh(#!/bin/sh
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
  cd / && FAKE_TUI_GREETING=$2 exec "$dir/fake_tui"
  ;;
bg)
  printf '%s\n%s\n' "$(pwd -P)" "$argv" > "$dir/bg.ran"
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
    void Start(const QJsonObject& sections = {}, bool withParts = true) {
        if (!sections.isEmpty()) {
            QVERIFY(Seed(sections));
        }
        Boot(4, FORM_CLAUDE);
        if (QTest::currentTestFailed() || !withParts) {
            return;
        }
        // A panel without the parts fails here, not on a null pointer.
        for (const char* name :
             {"sessionsSearch", "sessionsSearchInput", "sessionsNewButton",
              "sessionsNewForm", "sessionsNewTask", "sessionsNewName",
              "sessionsNewCategory", "sessionsNewCreate", "sessionsNewBack"}) {
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
        QTest::mouseClick(
            Window(), Qt::LeftButton, Qt::NoModifier,
            item->mapToScene(QPointF(item->width() / 2, item->height() / 2))
                .toPoint());
    }

    // "+ New", then waits until the task has the keyboard.
    void OpenForm() {
        Click(Part("sessionsNewButton"));
        QTRY_VERIFY_WITH_TIMEOUT(FormOpen(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(TaskField()->hasActiveFocus(), TIMEOUT_MS);
    }

    // Moves the keyboard to the name and waits for it.
    void ToName() {
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(NameField()->hasActiveFocus(), TIMEOUT_MS);
    }

    void VerifyListShown() {
        QVERIFY(!FormOpen());
        QVERIFY(!Form()->isVisible());
        QVERIFY(Part("sessionsHead")->isVisible());
        QVERIFY(ListView()->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);
    }

    void Snap(const QString& step) { QVERIFY(SaveSnapshot(Window(), step)); }

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
        Type("flaky");
        QTRY_COMPARE_WITH_TIMEOUT(Ids(), (QStringList{ALPHA}), TIMEOUT_MS);

        Press(Qt::Key_Down);

        QTRY_VERIFY_WITH_TIMEOUT(ListView()->hasActiveFocus(), TIMEOUT_MS);
        QCOMPARE(Query(), QString("flaky"));
        QCOMPARE(TextOf("sessionsSearchInput"), QString("flaky"));
        QCOMPARE(Ids(), (QStringList{ALPHA}));
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

        OpenForm();

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
        QVERIFY(NameField()->y() < CategoryBox()->y());
        QVERIFY(!CreateButton()->property("enabled").toBool());
        Snap("1-empty");
    }

    void TabAndBacktabWalkTheFieldsOfTheForm() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenForm();
        Type("Ship it");

        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(NameField()->hasActiveFocus(), TIMEOUT_MS);
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
        OpenForm();
        QVERIFY(!CategoryName()->isVisible());
        ToName();
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(CategoryBox()->hasActiveFocus(), TIMEOUT_MS);

        // Down on the closed box takes the next entry: "New category…".
        Press(Qt::Key_Down);

        QTRY_VERIFY_WITH_TIMEOUT(CategoryName()->isVisible(), TIMEOUT_MS);
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
        OpenForm();
        Type("Half");

        Press(Qt::Key_Escape);

        VerifyListShown();
        QVERIFY(BgRan().isEmpty());
        // A form opened again starts empty.
        OpenForm();
        QCOMPARE(TextOf("sessionsNewTask"), QString());
        QCOMPARE(TextOf("sessionsNewName"), QString());
    }

    void EscCancelsFromTheOtherControlsToo() {
        FailOnWarnings();
        Start();
        if (QTest::currentTestFailed()) {
            return;
        }
        OpenForm();
        Type("x");
        ToName();
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(CategoryBox()->hasActiveFocus(), TIMEOUT_MS);
        Press(Qt::Key_Escape);
        VerifyListShown();

        OpenForm();
        Type("x");
        ToName();
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
        OpenForm();
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
        OpenForm();
        Type("Ship it");
        ToName();
        Type("Shipping");
        Tab();
        Press(Qt::Key_Down);
        QTRY_VERIFY_WITH_TIMEOUT(CategoryName()->hasActiveFocus(), TIMEOUT_MS);
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
        OpenForm();
        Type("Ship it");
        ToName();
        Type("Shipped");
        Tab();
        QTRY_VERIFY_WITH_TIMEOUT(CategoryBox()->hasActiveFocus(), TIMEOUT_MS);
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
        QCOMPARE(Role(row, "category").toString(), QString("Ops"));
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
        OpenForm();
        Type("Ship it");
        ToName();
        Tab();
        Press(Qt::Key_Down);
        QTRY_VERIFY_WITH_TIMEOUT(CategoryName()->hasActiveFocus(), TIMEOUT_MS);
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
        OpenForm();
        Type("Ship it");
        // A task alone is enough.
        QTRY_VERIFY_WITH_TIMEOUT(CreateButton()->property("enabled").toBool(),
                                 TIMEOUT_MS);
        Snap("2-task-only");
        ToName();
        // A blank name is no name.
        Type("  ");
        Tab();
        Press(Qt::Key_Down);
        QTRY_VERIFY_WITH_TIMEOUT(CategoryName()->hasActiveFocus(), TIMEOUT_MS);
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
        OpenForm();
        QVERIFY(!CreateButton()->property("enabled").toBool());

        // A name is not enough.
        ToName();
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

        // Barrier: the form is still open, and what the list says is as it
        // was, after a round trip of the window.
        QVERIFY(FormOpen());
        QVERIFY(!CreateButton()->property("enabled").toBool());
        QCOMPARE(TextOf("sessionsNewName"), QString("Shipping"));
        QVERIFY(BgRan().isEmpty());
        QVERIFY(!Ids().contains(STARTED));
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
};

QTEST_MAIN(SessionsFormTest)
#include "sessions_form_test.moc"
