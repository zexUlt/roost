// The notifications plugin (M8): the real roostd with the real plugin and a
// fixture agent backend that reports the changes of state of its sessions. The
// commands of the user are small bash lines that write what they got into files
// of the test's own directory.

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <memory>

#include "core/rpc_client.hpp"
#include "helpers/roostd_process.hpp"
#include "helpers/scoped_env.hpp"

using namespace roost;
using roost::testing::RoostdProcess;
using roost::testing::ScopedEnv;

namespace {

constexpr int TIMEOUT_MS = 10000;
const QString AGENT = QStringLiteral("fix-agent");
const QString PLUGIN = QStringLiteral("notifications");

bool WaitFor(const QFuture<rpc::Result>& future) {
    if (future.isFinished()) {
        return true;
    }
    QEventLoop loop;
    QFutureWatcher<rpc::Result> watcher;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop,
                     &QEventLoop::quit);
    QTimer::singleShot(TIMEOUT_MS, &loop, &QEventLoop::quit);
    watcher.setFuture(future);
    loop.exec();
    return future.isFinished();
}

QString MessageOf(const rpc::Result& result) {
    return result ? QString() : result.error().Message;
}

// What the fixture backend sends roostd as the change of a session.
QJsonObject Report(const QString& id, const QString& state,
                   const QString& title = {}, const QString& cwd = {}) {
    return {
        {"jsonrpc", "2.0"},
        {"method", "session.stateChanged"},
        {"params", QJsonObject{{"agent", AGENT},
                               {"session", QJsonObject{{"id", id},
                                                       {"title", title},
                                                       {"cwd", cwd},
                                                       {"state", state}}}}}};
}

bool IsRunning(qint64 pid) {
    QProcess ps;
    ps.start("ps", {"-o", "stat=", "-p", QString::number(pid)});
    if (!ps.waitForFinished(TIMEOUT_MS)) {
        return true;
    }
    const QByteArray state = ps.readAllStandardOutput().trimmed();
    // A zombie of a process nobody reaps is gone for this purpose.
    return !state.isEmpty() && !state.startsWith('Z');
}

}  // namespace

class NotificationsTest : public QObject {
    Q_OBJECT

private:
    QString Path(const QString& name) const { return Work_->filePath(name); }

    // The text of a command that appends "<event>:<session>" to <name>.log.
    QString LogCommand(const QString& name) const {
        return QStringLiteral("echo \"$ROOST_EVENT:$ROOST_SESSION_ID\" >> '%1'")
            .arg(Path(name + ".log"));
    }

    QJsonObject Command(const QString& name, const QJsonArray& events = {},
                        const QJsonValue& enabled = QJsonValue::Undefined) {
        QJsonObject command{{"name", name}, {"run", LogCommand(name)}};
        if (!events.isEmpty()) {
            command.insert("events", events);
        }
        if (!enabled.isUndefined()) {
            command.insert("enabled", enabled);
        }
        return command;
    }

    // The lines of a file, sorted: commands of different events run side by
    // side and finish in any order.
    QStringList Lines(const QString& name) const {
        QFile file(Path(name));
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        QStringList lines =
            QString::fromUtf8(file.readAll()).split(u'\n', Qt::SkipEmptyParts);
        lines.sort();
        return lines;
    }

    QByteArray Content(const QString& name) const {
        QFile file(Path(name));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    // The real roostd, the plugin as built and the fixture agent.
    bool Start() {
        Roostd_ = std::make_unique<RoostdProcess>();
        if (!Roostd_->IsValid() ||
            !Roostd_->AddFixturePlugin("fix", FIXTURE_BACKEND_PATH) ||
            !QFile::link(NOTIFICATIONS_PLUGIN_DIR,
                         Roostd_->PluginsPath() + "/notifications") ||
            !Roostd_->Start(ROOSTD_PATH)) {
            return false;
        }
        Client_ = std::make_unique<RpcClient>();
        const QFuture<rpc::Result> hello =
            Client_->Connect(Roostd_->SocketPath());
        if (!WaitFor(hello) || !hello.result()) {
            return false;
        }
        // roostd has read what the backend asked for at its start, in order,
        // before it answers anything else the backend says: from now on the
        // backend is subscribed, and a state it is told is seen.
        bool answered = false;
        for (int i = 0; i < 100 && !answered; ++i) {
            answered = Test().has_value();
            if (!answered) {
                QTest::qWait(50);
            }
        }
        return answered;
    }

    rpc::Result Call(const QString& method, const QJsonValue& params) {
        const QFuture<rpc::Result> future = Client_->Request(method, params);
        if (!WaitFor(future)) {
            return std::unexpected(rpc::Error{.Message = "timed out"});
        }
        return future.result();
    }

    // notifications.test, with the name of a command or for all.
    rpc::Result Test(const QString& name = {}) {
        QJsonObject params{{"command", "notifications.test"}};
        if (!name.isEmpty()) {
            params.insert("args", QJsonObject{{"name", name}});
        }
        return Call("commands.run", params);
    }

    // Has the fixture backend report these, in order. roostd has published
    // them when this returns.
    void Reports(const QJsonArray& messages) {
        const rpc::Result sent = Call(
            "commands.run",
            QJsonObject{{"command", "fixture.notify"}, {"args", messages}});
        QVERIFY2(sent, qPrintable(MessageOf(sent)));
    }

    // Has the fixture backend die, and waits until roostd has started it again
    // and it answers: roostd has announced the restart by then.
    void RestartAdapter() {
        // The backend exits without answering: an error, or nothing.
        Call("commands.run", QJsonObject{{"command", "fixture.crash"}});
        QTRY_VERIFY_WITH_TIMEOUT(
            Call("commands.run", QJsonObject{{"command", "fixture.notify"},
                                             {"args", QJsonArray{}}})
                .has_value(),
            TIMEOUT_MS);
    }

    // Replaces `settings` and leaves `results` alone, as the settings page.
    void PutSettings(const QJsonObject& settings) {
        const rpc::Result patched =
            Call("store.patch",
                 QJsonObject{{"plugin", PLUGIN},
                             {"patch", QJsonObject{{"settings", settings}}}});
        QVERIFY2(patched, qPrintable(MessageOf(patched)));
    }

    QJsonObject Document() {
        const rpc::Result got =
            Call("store.get", QJsonObject{{"plugin", PLUGIN}});
        return got ? got->toObject().value("document").toObject()
                   : QJsonObject();
    }

    QJsonObject Results() { return Document().value("results").toObject(); }

    // One command at a time, in the order they were queued: a command added
    // last is a barrier that runs after all before it, so that "nothing ran"
    // is checked after the runs that could have been wrong.
    std::unique_ptr<ScopedEnv> OneAtATime_;
    std::unique_ptr<QTemporaryDir> Work_;
    std::unique_ptr<RoostdProcess> Roostd_;
    std::unique_ptr<RpcClient> Client_;

private slots:
    void init() {
        OneAtATime_ = std::make_unique<ScopedEnv>(
            "ROOST_NOTIFICATIONS_TEST_MAX_RUNNING", "1");
        Work_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Work_->isValid());
    }

    void cleanup() {
        Client_.reset();
        Roostd_.reset();
        OneAtATime_.reset();
    }

    void ABlockedSessionRunsTheCommandOnceWithTheEnvironmentAndStdin() {
        QVERIFY(Start());
        // One file of each kind for each session: two sessions run it.
        const QString run = QStringLiteral(
            "echo \"$ROOST_SESSION_ID\" >> '%1'; for v in ROOST_NOTIFY_VERSION "
            "ROOST_EVENT ROOST_AGENT ROOST_SESSION_ID ROOST_SESSION_TITLE "
            "ROOST_SESSION_CWD ROOST_STATE ROOST_MESSAGE; do printf '%s=%s\\n' "
            "\"$v\" \"${!v}\"; done > '%2/env.'\"$ROOST_SESSION_ID\"; cat > "
            "'%2/stdin.'\"$ROOST_SESSION_ID\"");
        PutSettings({{"commands",
                      QJsonArray{QJsonObject{{"name", "rec"},
                                             {"run", run.arg(Path("runs"),
                                                             Work_->path())},
                                             {"events", QJsonArray{"blocked"}}},
                                 QJsonObject{{"name", "fail"},
                                             {"run", "echo oops >&2; exit 3"},
                                             {"events", QJsonArray{"blocked"}}},
                                 Command("barrier", {"failed"})}}});

        // The adapters send only changes, so a session first reported as
        // blocked is a change: it runs, as s1 does when it turns blocked. The
        // same state again runs nothing.
        Reports({Report("born-blocked", "blocked", "Fresh"),
                 Report("s1", "working", "Fix the build", "/work/tree"),
                 Report("s1", "blocked", "Fix the build", "/work/tree"),
                 Report("s1", "blocked", "Fix the build", "/work/tree")});
        // The barrier runs after every run the events before it caused, and
        // so does its result.
        Reports({Report("b", "working"), Report("b", "failed")});
        QTRY_VERIFY_WITH_TIMEOUT(Results().contains("barrier") &&
                                     Results().contains("rec") &&
                                     Results().contains("fail"),
                                 TIMEOUT_MS);

        QCOMPARE(Lines("runs"), (QStringList{"born-blocked", "s1"}));
        QStringList env{"ROOST_NOTIFY_VERSION=1",
                        "ROOST_EVENT=blocked",
                        "ROOST_AGENT=" + AGENT,
                        "ROOST_SESSION_ID=s1",
                        "ROOST_SESSION_TITLE=Fix the build",
                        "ROOST_SESSION_CWD=/work/tree",
                        "ROOST_STATE=blocked",
                        "ROOST_MESSAGE=Fix the build needs you"};
        env.sort();
        QCOMPARE(Lines("env.s1"), env);
        QCOMPARE(QJsonDocument::fromJson(Content("stdin.s1")).object(),
                 (QJsonObject{{"version", 1},
                              {"event", "blocked"},
                              {"agent", AGENT},
                              {"sessionId", "s1"},
                              {"title", "Fix the build"},
                              {"cwd", "/work/tree"},
                              {"state", "blocked"},
                              {"message", "Fix the build needs you"}}));
        QCOMPARE(QJsonDocument::fromJson(Content("stdin.born-blocked"))
                     .object()
                     .value("message")
                     .toString(),
                 "Fresh needs you");

        // What the settings page shows: the last result of each command.
        const QJsonObject results = Results();
        const QJsonObject rec = results.value("rec").toObject();
        QCOMPARE(rec.value("exitCode").toInt(-1), 0);
        QVERIFY(!rec.contains("error"));
        QCOMPARE(rec.value("event").toString(), "blocked");
        QVERIFY(rec.value("at").toDouble() > 1e12);
        const QJsonObject fail = results.value("fail").toObject();
        QCOMPARE(fail.value("exitCode").toInt(-1), 3);
        QCOMPARE(fail.value("stderr").toString(), "oops\n");
        // The results did not touch what the user wrote.
        QCOMPARE(Document()
                     .value("settings")
                     .toObject()
                     .value("commands")
                     .toArray()
                     .size(),
                 3);
    }

    void NothingOfASessionIsRunAsShell() {
        QVERIFY(Start());
        const QString pwned = Path("pwned");
        // All the quotes at once, to arrive intact; then each way to run
        // something from a title that a command line that took it would.
        const QString nasty = QStringLiteral("a\"b'c`d` $HOME \\ \n x");
        const QString cwd =
            QStringLiteral("/tmp/it's \"x\"/$(touch %1)").arg(pwned);
        const QString run = QStringLiteral(
            "printf '%s\\n' \"$ROOST_SESSION_ID\" >> '%1'; printf '%s' "
            "\"$ROOST_SESSION_TITLE\" > '%2/title.'\"$ROOST_SESSION_ID\"; "
            "printf '%s' \"$ROOST_SESSION_CWD\" > '%2/cwd'; printf '%s' "
            "\"$ROOST_MESSAGE\" > '%2/message.'\"$ROOST_SESSION_ID\"; cat > "
            "'%2/stdin.'\"$ROOST_SESSION_ID\"");
        PutSettings({{"commands",
                      QJsonArray{QJsonObject{{"name", "rec"},
                                             {"run", run.arg(Path("ids"),
                                                             Work_->path())},
                                             {"events", QJsonArray{"blocked"}}},
                                 Command("barrier", {"failed"})}}});
        const QMap<QString, QString> titles{
            {"nasty", nasty},
            {"subst", QStringLiteral("$(touch %1)").arg(pwned)},
            {"backtick", QStringLiteral("`touch %1`").arg(pwned)},
            {"chain", QStringLiteral("\"; touch %1; echo \"").arg(pwned)},
            {"single", QStringLiteral("'; touch %1; echo '").arg(pwned)}};
        QJsonArray reports;
        for (auto it = titles.begin(); it != titles.end(); ++it) {
            reports.append(Report(it.key(), "working", it.value(), cwd));
            reports.append(Report(it.key(), "blocked", it.value(), cwd));
        }
        Reports(reports);
        Reports({Report("b", "working"), Report("b", "failed")});
        QTRY_VERIFY_WITH_TIMEOUT(Results().contains("barrier") &&
                                     Lines("ids").size() == titles.size(),
                                 TIMEOUT_MS);
        // Every run is over: reading its stdin to the end is the last thing it
        // does, and the file is whole JSON then.
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (const QString& id : titles.keys()) {
                    if (QJsonDocument::fromJson(Content("stdin." + id))
                            .isNull()) {
                        return false;
                    }
                }
                return true;
            }(),
            TIMEOUT_MS);

        QVERIFY(!QFile::exists(pwned));
        for (auto it = titles.begin(); it != titles.end(); ++it) {
            const QString& id = it.key();
            QCOMPARE(QString::fromUtf8(Content("title." + id)), it.value());
            QCOMPARE(QString::fromUtf8(Content("message." + id)),
                     it.value() + " needs you");
            const QJsonObject input =
                QJsonDocument::fromJson(Content("stdin." + id)).object();
            QCOMPARE(input.value("title").toString(), it.value());
            QCOMPARE(input.value("cwd").toString(), cwd);
            QCOMPARE(input.value("message").toString(),
                     it.value() + " needs you");
        }
        QCOMPARE(QString::fromUtf8(Content("cwd")), cwd);
    }

    void ACommandThatRunsPastTheTimeoutIsKilledWithItsGroup() {
        // The 10 s of a command are too long for a test.
        const ScopedEnv timeout("ROOST_NOTIFICATIONS_TEST_TIMEOUT_MS", "5000");
        QVERIFY(Start());
        PutSettings(
            {{"commands",
              QJsonArray{QJsonObject{
                  {"name", "hang"},
                  {"run",
                   QStringLiteral("sleep 600 & echo $! > '%1'; echo $$ > "
                                  "'%2'; wait")
                       .arg(Path("child.pid"), Path("shell.pid"))},
                  {"events", QJsonArray{"blocked"}}}}}});
        Reports({Report("s1", "working"), Report("s1", "blocked")});
        QTRY_VERIFY_WITH_TIMEOUT(
            !Lines("child.pid").isEmpty() && !Lines("shell.pid").isEmpty(),
            TIMEOUT_MS);
        const qint64 child = Lines("child.pid").first().toLongLong();
        const qint64 shell = Lines("shell.pid").first().toLongLong();
        QVERIFY(child > 1 && shell > 1);
        QVERIFY(IsRunning(child));

        QTRY_VERIFY_WITH_TIMEOUT(Results().contains("hang"), 3 * TIMEOUT_MS);
        const QJsonObject hang = Results().value("hang").toObject();
        QCOMPARE(hang.value("error").toString(), "timeout");
        QVERIFY(!hang.contains("exitCode"));
        QCOMPARE(hang.value("event").toString(), "blocked");
        QTRY_VERIFY_WITH_TIMEOUT(!IsRunning(child) && !IsRunning(shell),
                                 TIMEOUT_MS);
    }

    void TheEventsOfTheSettingsAndOfACommandFilterTheRuns() {
        QVERIFY(Start());
        PutSettings(
            {{"events", QJsonArray{"blocked", "done"}},
             {"commands",
              QJsonArray{
                  Command("all"), Command("blocked-only", {"blocked"}),
                  // The settings leave failed out, whatever it says.
                  Command("failed-too", {"blocked", "done", "failed"}, true),
                  Command("failed-only", {"failed"}), Command("off", {}, false),
                  // Last in the order of a run: it runs after the others for
                  // the last event, so that all that could run has run.
                  Command("barrier", {"done"})}}});
        Reports({Report("s", "working"), Report("s", "blocked"),
                 Report("s", "done"), Report("s", "failed"),
                 Report("barrier", "working"), Report("barrier", "done")});
        QTRY_COMPARE_WITH_TIMEOUT(Lines("barrier.log"),
                                  (QStringList{"done:barrier", "done:s"}),
                                  TIMEOUT_MS);

        const QStringList all{"blocked:s", "done:barrier", "done:s"};
        QCOMPARE(Lines("all.log"), all);
        QCOMPARE(Lines("failed-too.log"), all);
        QCOMPARE(Lines("blocked-only.log"), QStringList{"blocked:s"});
        QVERIFY(!QFile::exists(Path("failed-only.log")));
        QVERIFY(!QFile::exists(Path("off.log")));
    }

    void TheTestCommandRunsOneCommandOrEveryEnabledOne() {
        QVERIFY(Start());
        const rpc::Result none = Test();
        QVERIFY2(none, qPrintable(MessageOf(none)));
        QCOMPARE(none->toObject().value("ran").toArray(), QJsonArray{});

        const QString run = QStringLiteral(
            "echo \"$ROOST_EVENT|$ROOST_MESSAGE|$ROOST_SESSION_ID\" >> '%1'; "
            "cat > '%2'");
        PutSettings({{"commands",
                      QJsonArray{QJsonObject{{"name", "a"},
                                             {"run", run.arg(Path("a.log"),
                                                             Path("a.stdin"))},
                                             // The filters are not asked.
                                             {"events", QJsonArray{"done"}}},
                                 Command("off", {}, false), Command("b")}}});

        const rpc::Result named = Test("a");
        QVERIFY2(named, qPrintable(MessageOf(named)));
        QCOMPARE(named->toObject().value("ran").toArray(), QJsonArray{"a"});
        QTRY_COMPARE_WITH_TIMEOUT(
            Lines("a.log"), QStringList{"test|Test notification from roost|"},
            TIMEOUT_MS);
        const QJsonObject input =
            QJsonDocument::fromJson(Content("a.stdin")).object();
        QCOMPARE(input.value("event").toString(), "test");
        QCOMPARE(input.value("message").toString(),
                 "Test notification from roost");

        const rpc::Result every = Test();
        QVERIFY2(every, qPrintable(MessageOf(every)));
        QCOMPARE(every->toObject().value("ran").toArray(),
                 (QJsonArray{"a", "b"}));
        // One at a time, in the order of the settings: when b, the last, has
        // run, off would have.
        QTRY_VERIFY_WITH_TIMEOUT(Lines("b.log") == QStringList{"test:"},
                                 TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Lines("a.log").size(), 2, TIMEOUT_MS);
        QVERIFY(!QFile::exists(Path("off.log")));

        // A disabled command is run when it is asked for by name.
        const rpc::Result off = Test("off");
        QVERIFY2(off, qPrintable(MessageOf(off)));
        QCOMPARE(off->toObject().value("ran").toArray(), QJsonArray{"off"});
        QTRY_COMPARE_WITH_TIMEOUT(Lines("off.log"), QStringList{"test:"},
                                  TIMEOUT_MS);

        const rpc::Result unknown = Test("nope");
        QVERIFY(!unknown);
        QCOMPARE(unknown.error().Code,
                 static_cast<int>(rpc::EErrorCode::InvalidParams));
        QVERIFY(unknown.error().Message.contains("nope"));
        const rpc::Result other = Call(
            "commands.run", QJsonObject{{"command", "notifications.nothing"}});
        QVERIFY(!other);
    }

    void ASettingsChangeIsSeenWithoutARestart() {
        QVERIFY(Start());
        PutSettings({{"commands", QJsonArray{Command("one", {"blocked"})}}});
        Reports({Report("s1", "working"), Report("s1", "blocked")});
        QTRY_COMPARE_WITH_TIMEOUT(Lines("one.log"), QStringList{"blocked:s1"},
                                  TIMEOUT_MS);

        // Through the socket, as the settings page: another command, and the
        // events of the whole.
        PutSettings({{"events", QJsonArray{"done"}},
                     {"commands", QJsonArray{Command("two", {"done"})}}});
        Reports({Report("s2", "working"), Report("s2", "blocked"),
                 Report("s2", "done")});
        QTRY_COMPARE_WITH_TIMEOUT(Lines("two.log"), QStringList{"done:s2"},
                                  TIMEOUT_MS);
        // one is gone, and blocked is out.
        QCOMPARE(Lines("one.log"), QStringList{"blocked:s1"});
        // The result of a command that is gone goes with it.
        QTRY_VERIFY_WITH_TIMEOUT(
            !Results().contains("one") && Results().contains("two"),
            TIMEOUT_MS);
    }

    void ABadFieldIsSkippedAndTheRestWorks() {
        QVERIFY(Start());
        PutSettings(
            {{"desktop", "yes"},
             {"events", "blocked"},
             {"commands",
              QJsonArray{QJsonObject{{"name", ""}, {"run", "true"}},
                         QJsonObject{{"name", "no-run"}}, QJsonValue(5),
                         Command("good", QJsonArray{"blocked", "bogus"}),
                         Command("good", {"blocked"}),
                         QJsonObject{{"name", "bad-enabled"},
                                     {"run", LogCommand("bad-enabled")},
                                     {"enabled", "no"}}}}});
        Reports({Report("s", "working"), Report("s", "blocked")});
        // The defaults of `events` hold: blocked is in them.
        QTRY_VERIFY_WITH_TIMEOUT(Lines("bad-enabled.log").size() == 1 &&
                                     Lines("good.log").size() == 1,
                                 TIMEOUT_MS);
        QCOMPARE(Lines("good.log"), QStringList{"blocked:s"});
    }

    void ARemovedSessionIsForgotten() {
        QVERIFY(Start());
        PutSettings({{"commands", QJsonArray{Command("c", {"blocked"}),
                                             Command("barrier", {"failed"})}}});
        Reports({Report("s", "working"), Report("s", "blocked"),
                 QJsonObject{{"jsonrpc", "2.0"},
                             {"method", "session.removed"},
                             {"params",
                              QJsonObject{{"agent", AGENT}, {"session", "s"}}}},
                 // The same id again is a new session: blocked is its first
                 // state, a change, and runs again. Without the removal it
                 // would be the same state.
                 Report("s", "blocked"), Report("s", "blocked")});
        Reports({Report("b", "working"), Report("b", "failed")});
        QTRY_VERIFY_WITH_TIMEOUT(Lines("barrier.log").size() == 1, TIMEOUT_MS);
        QCOMPARE(Lines("c.log"), (QStringList{"blocked:s", "blocked:s"}));
    }

    void ARestartOfTheAdapterForgetsTheStatesOfItsAgents() {
        QVERIFY(Start());
        PutSettings({{"commands", QJsonArray{Command("c", {"blocked"}),
                                             Command("barrier", {"failed"})}}});
        Reports({Report("s", "working"), Report("s", "blocked")});
        QTRY_COMPARE_WITH_TIMEOUT(Lines("c.log"), QStringList{"blocked:s"},
                                  TIMEOUT_MS);

        // The adapter does not tell what changed while it was down: the
        // session may have worked and been blocked again. Its next report of
        // blocked is a change, not a repeat.
        RestartAdapter();
        Reports({Report("s", "blocked")});
        Reports({Report("b", "working"), Report("b", "failed")});
        QTRY_VERIFY_WITH_TIMEOUT(Lines("barrier.log").size() == 1, TIMEOUT_MS);
        QCOMPARE(Lines("c.log"), (QStringList{"blocked:s", "blocked:s"}));
    }
};

QTEST_GUILESS_MAIN(NotificationsTest)
#include "notifications_test.moc"
