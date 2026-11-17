// The Claude adapter: the parsing of `claude agents --json --all`, then the
// real claude-adapter binary over stdio with a fake `claude` first on PATH.
// With ROOST_REAL_CLAUDE=1 it also lists the sessions of the real one.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

#include <csignal>

#include <cstddef>
#include <memory>
#include <optional>

#include "claude/backend/agents.hpp"

using namespace roost::claude;

namespace {

constexpr int TIMEOUT_MS = 5000;

QByteArray ReadFixture() {
    QFile file(QStringLiteral(FIXTURES_DIR "/claude_agents.json"));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray Agents(const QJsonArray& entries) {
    return QJsonDocument(entries).toJson(QJsonDocument::Compact);
}

QJsonObject Entry(const QString& id, const QString& state) {
    return {{"id", id}, {"kind", "background"}, {"state", state}};
}

QJsonObject Request(qint64 id, const QString& method,
                    const QJsonObject& params) {
    return {
        {"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}

// A fake `claude`, steered by files next to it:
//   agents.json  what it prints
//   stderr       what it prints to stderr
//   exit         its exit code
//   sleep        seconds to take
//   hang         if present, writes its pid to pid and sleeps for good
// Each run adds "start <args>" and "end" lines to calls.
constexpr const char* FAKE_CLAUDE = R"sh(#!/bin/sh
dir=$(dirname "$0")
echo "start $*" >> "$dir/calls"
if [ -f "$dir/sigpipe" ]; then ( yes 2>/dev/null; echo $? > "$dir/yes" ) | true; fi
if [ -f "$dir/hang" ]; then
  sleep 600 & echo $! >> "$dir/pids"; echo $$ >> "$dir/pids"; exec sleep 600
fi
if [ -f "$dir/sleep" ]; then sleep "$(cat "$dir/sleep")"; fi
if [ -f "$dir/stderr" ]; then cat "$dir/stderr" >&2; fi
if [ -f "$dir/agents.json" ]; then cat "$dir/agents.json"; fi
echo end >> "$dir/calls"
exit "$(cat "$dir/exit" 2>/dev/null || echo 0)"
)sh";

}  // namespace

class ClaudeAdapterTest : public QObject {
    Q_OBJECT

private:
    QString FakePath(const QString& name) const {
        return Dir_->filePath("bin/" + name);
    }

    void Put(const QString& name, const QByteArray& content) {
        QFile file(FakePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(content), content.size());
    }

    QStringList Calls() const {
        QFile file(FakePath("calls"));
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QString::fromUtf8(file.readAll())
            .split(u'\n', Qt::SkipEmptyParts);
    }

    enum class EClaude { Fake, None, Real };

    // Starts the adapter with the fake claude first on PATH, with no claude
    // at all or with the real one.
    void StartAdapter(const QStringList& args = {},
                      EClaude claude = EClaude::Fake) {
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        if (claude == EClaude::Fake) {
            environment.insert("PATH", Dir_->filePath("bin") + u':' +
                                           environment.value("PATH"));
        } else if (claude == EClaude::None) {
            environment.insert("PATH", Dir_->filePath("empty"));
        }
        Adapter_ = std::make_unique<QProcess>();
        Adapter_->setProcessEnvironment(environment);
        Adapter_->setProcessChannelMode(QProcess::ForwardedErrorChannel);
        QStringList all{"--poll-ms", "20"};
        all.append(args);
        Adapter_->start(CLAUDE_ADAPTER_PATH, all);
        QVERIFY(Adapter_->waitForStarted(TIMEOUT_MS));
    }

    void Write(const QJsonObject& message) {
        Adapter_->write(QJsonDocument(message).toJson(QJsonDocument::Compact) +
                        '\n');
    }

    // Reads whatever full lines the adapter has sent.
    void Drain() {
        Buffer_.append(Adapter_->readAllStandardOutput());
        for (qsizetype end = Buffer_.indexOf('\n'); end >= 0;
             end = Buffer_.indexOf('\n')) {
            Received_.append(QJsonDocument::fromJson(Buffer_.first(end)));
            Buffer_.remove(0, end + 1);
        }
    }

    std::optional<QJsonObject> Response(qint64 id) {
        Drain();
        for (const QJsonDocument& message : std::as_const(Received_)) {
            if (message.object().value("id") == QJsonValue(id)) {
                return message.object();
            }
        }
        return std::nullopt;
    }

    QJsonObject Ask(const QString& method, const QJsonObject& params) {
        const qint64 id = NextId_++;
        Write(Request(id, method, params));
        std::optional<QJsonObject> response;
        const bool answered = QTest::qWaitFor(
            [&] {
                Adapter_->waitForReadyRead(10);
                response = Response(id);
                return response.has_value();
            },
            TIMEOUT_MS);
        return answered ? *response : QJsonObject{{"timeout", true}};
    }

    QJsonValue ListSessions() {
        const QJsonObject response =
            Ask("agent.listSessions", {{"agent", "claude"}});
        return response.contains("result") ? response.value("result")
                                           : response.value("error");
    }

    QList<QJsonObject> Notifications() {
        Drain();
        QList<QJsonObject> found;
        for (const QJsonDocument& message : std::as_const(Received_)) {
            if (message.object().contains("method")) {
                found.append(message.object());
            }
        }
        return found;
    }

    // The removals among the notifications, as {agent, session} lines.
    static QStringList Removals(const QList<QJsonObject>& sent) {
        QStringList removed;
        for (const QJsonObject& message : sent) {
            if (message.value("method") == QJsonValue("session.removed")) {
                const QJsonObject params = message.value("params").toObject();
                removed.append(params.value("agent").toString() + u'/' +
                               params.value("session").toString());
            }
        }
        return removed;
    }

    // Waits until the fake has finished this many runs since the call.
    bool WaitForRuns(int runs) {
        const qsizetype before = Calls().count("end");
        return QTest::qWaitFor(
            [&] {
                Adapter_->waitForReadyRead(10);
                return Calls().count("end") >= before + runs;
            },
            TIMEOUT_MS);
    }

    std::unique_ptr<QTemporaryDir> Dir_;
    std::unique_ptr<QProcess> Adapter_;
    QByteArray Buffer_;
    QList<QJsonDocument> Received_;
    qint64 NextId_ = 1;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
        QVERIFY(QDir(Dir_->path()).mkpath("bin"));
        QVERIFY(QDir(Dir_->path()).mkpath("empty"));
        Put("claude", FAKE_CLAUDE);
        QVERIFY(QFile::setPermissions(
            FakePath("claude"),
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        Buffer_.clear();
        Received_.clear();
        NextId_ = 1;
    }

    void cleanup() {
        if (Adapter_) {
            // Let it stop its run of claude; a killed adapter leaves it.
            Adapter_->closeWriteChannel();
            if (!Adapter_->waitForFinished(TIMEOUT_MS)) {
                Adapter_->kill();
                Adapter_->waitForFinished(TIMEOUT_MS);
            }
            Adapter_.reset();
        }
    }

    // --- Parsing ---

    void TheFixtureIsReadIntoBackgroundSessions() {
        const auto listing = ParseAgents(ReadFixture());
        QVERIFY2(listing, qPrintable(listing ? QString() : listing.error()));
        QCOMPARE(listing->Warnings, QStringList());
        const std::vector<Session> expected{
            {.Id = "bg-0a1b2c3d",
             .Title = "Fix the flaky test",
             .WorkingDirectory = "/work/alpha",
             .State = "working",
             .StartedAt = 1790150000000},
            {.Id = "bg-1b2c3d4e",
             .Title = "Review the PR",
             .WorkingDirectory = "/work/beta",
             .State = "blocked",
             .StartedAt = 1790150100000},
            // No name: the id is the title.
            {.Id = "bg-2c3d4e5f",
             .Title = "bg-2c3d4e5f",
             .WorkingDirectory = "/work/gamma",
             .State = "done",
             .StartedAt = 1790150200000},
            // Done, but busy with a new turn.
            {.Id = "bg-3d4e5f60",
             .Title = "Answer a question",
             .WorkingDirectory = "/work/delta",
             .State = "working",
             .StartedAt = 1790150300000},
        };
        QCOMPARE(listing->Sessions, expected);
    }

    void StatesAreMapped_data() {
        QTest::addColumn<QString>("state");
        QTest::addColumn<QString>("status");
        QTest::addColumn<QString>("mapped");
        QTest::addColumn<bool>("warned");
        QTest::newRow("working") << "working" << "" << "working" << false;
        QTest::newRow("blocked") << "blocked" << "" << "blocked" << false;
        QTest::newRow("idle") << "idle" << "" << "idle" << false;
        QTest::newRow("failed") << "failed" << "" << "failed" << false;
        QTest::newRow("needs_input")
            << "needs_input" << "" << "blocked" << false;
        QTest::newRow("done") << "done" << "" << "done" << false;
        QTest::newRow("done, idle") << "done" << "idle" << "done" << false;
        QTest::newRow("done, busy") << "done" << "busy" << "working" << false;
        QTest::newRow("working, idle")
            << "working" << "idle" << "working" << false;
        QTest::newRow("unknown") << "pondering" << "" << "idle" << true;
        QTest::newRow("none") << "" << "" << "idle" << true;
    }

    void StatesAreMapped() {
        QFETCH(QString, state);
        QFETCH(QString, status);
        QFETCH(QString, mapped);
        QFETCH(bool, warned);
        QJsonObject entry{{"id", "s1"}, {"kind", "background"}};
        if (!state.isEmpty()) {
            entry.insert("state", state);
        }
        if (!status.isEmpty()) {
            entry.insert("status", status);
        }
        const auto listing = ParseAgents(Agents({entry}));
        QVERIFY(listing);
        QCOMPARE(listing->Sessions.size(), std::size_t{1});
        QCOMPARE(listing->Sessions[0].State, mapped);
        QCOMPARE(listing->Warnings.size(), qsizetype{warned ? 1 : 0});
        if (warned) {
            QCOMPARE(listing->Warnings[0],
                     QString("session s1 is in state \"%1\", taken as idle")
                         .arg(state));
        }
    }

    void OnlyBackgroundSessionsWithAnIdAreKept() {
        const auto listing = ParseAgents(Agents({
            Entry("a", "working"),
            QJsonObject{{"id", "b"}, {"kind", "interactive"}},
            QJsonObject{{"id", "c"}, {"state", "working"}},
            QJsonObject{{"kind", "background"}, {"state", "working"}},
            QJsonObject{{"id", 7}, {"kind", "background"}},
            QJsonObject{{"id", ""}, {"kind", "background"}},
            "not an object",
            QJsonValue(),
            Entry("d", "blocked"),
            Entry("a", "done"),
        }));
        QVERIFY(listing);
        QCOMPARE(listing->Sessions.size(), std::size_t{2});
        QCOMPARE(listing->Sessions[0].Id, QString("a"));
        QCOMPARE(listing->Sessions[0].State, QString("working"));
        QCOMPARE(listing->Sessions[1].Id, QString("d"));
        QCOMPARE(listing->Warnings,
                 (QStringList{
                     "a background session without an id is left out",
                     "a background session without an id is left out",
                     "a background session without an id is left out",
                     "session a is listed twice; the first one is kept",
                 }));
    }

    void BadFieldsAreLeftOut_data() {
        QTest::addColumn<QJsonValue>("startedAt");
        QTest::addColumn<qint64>("expected");
        QTest::newRow("an integer")
            << QJsonValue(qint64{1790150000000}) << qint64{1790150000000};
        QTest::newRow("a whole double")
            << QJsonValue(1790150000000.0) << qint64{1790150000000};
        QTest::newRow("a fraction") << QJsonValue(1.5) << qint64{0};
        QTest::newRow("zero") << QJsonValue(0) << qint64{0};
        QTest::newRow("negative") << QJsonValue(-5) << qint64{0};
        QTest::newRow("a string") << QJsonValue("1790150000000") << qint64{0};
        QTest::newRow("absent")
            << QJsonValue(QJsonValue::Undefined) << qint64{0};
    }

    void BadFieldsAreLeftOut() {
        QFETCH(QJsonValue, startedAt);
        QFETCH(qint64, expected);
        QJsonObject entry = Entry("s1", "idle");
        entry.insert("startedAt", startedAt);
        entry.insert("name", 3);
        entry.insert("cwd", QJsonArray{"/w"});
        const auto listing = ParseAgents(Agents({entry}));
        QVERIFY(listing);
        QCOMPARE(listing->Sessions[0].StartedAt, expected);
        QCOMPARE(listing->Sessions[0].Title, QString("s1"));
        QCOMPARE(listing->Sessions[0].WorkingDirectory, QString());
    }

    void OutputThatIsNoArrayFails_data() {
        QTest::addColumn<QByteArray>("output");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("not JSON") << QByteArray("Error: not logged in\n");
        QTest::newRow("an object") << QByteArray(R"({"agents": []})");
        QTest::newRow("cut short") << ReadFixture().first(100);
    }

    void OutputThatIsNoArrayFails() {
        QFETCH(QByteArray, output);
        const auto listing = ParseAgents(output);
        QVERIFY(!listing);
        QCOMPARE(listing.error(),
                 QString("claude agents printed no JSON array"));
    }

    void AnEmptyListIsFine() {
        const auto listing = ParseAgents("[]\n");
        QVERIFY(listing);
        QVERIFY(listing->Sessions.empty());
    }

    void ASessionGoesToJson() {
        QCOMPARE(ToJson({.Id = "s1",
                         .Title = "t",
                         .WorkingDirectory = "/w",
                         .State = "done",
                         .StartedAt = 5}),
                 (QJsonObject{{"id", "s1"},
                              {"state", "done"},
                              {"title", "t"},
                              {"cwd", "/w"},
                              {"startedAt", 5}}));
        QCOMPARE(ToJson({.Id = "s1",
                         .Title = "s1",
                         .WorkingDirectory = "",
                         .State = "idle",
                         .StartedAt = 0}),
                 (QJsonObject{{"id", "s1"},
                              {"state", "idle"},
                              {"title", "s1"},
                              {"cwd", ""}}));
    }

    // --- The adapter over stdio ---

    void ListsWhatClaudeAgentsPrints() {
        Put("agents.json", ReadFixture());
        StartAdapter();
        const QJsonValue listed = ListSessions();
        const auto listing = ParseAgents(ReadFixture());
        QVERIFY(listing);
        QJsonArray expected;
        for (const Session& session : listing->Sessions) {
            expected.append(ToJson(session));
        }
        QCOMPARE(listed, QJsonValue(expected));
        QCOMPARE(Calls().value(0), QString("start agents --json --all"));
    }

    void ARequestWaitsForTheFirstRun() {
        Put("agents.json", Agents({Entry("s1", "idle")}));
        Put("sleep", "0.5");
        StartAdapter();
        QCOMPARE(ListSessions(),
                 QJsonValue(QJsonArray{ToJson({.Id = "s1",
                                               .Title = "s1",
                                               .WorkingDirectory = "",
                                               .State = "idle",
                                               .StartedAt = 0})}));
    }

    void AttachRunsClaudeAttach() {
        Put("agents.json", ReadFixture());
        StartAdapter();
        QCOMPARE(Ask("agent.attachCommand",
                     {{"agent", "claude"}, {"session", "bg-1b2c3d4e"}})
                     .value("result"),
                 QJsonValue(QJsonObject{
                     {"program", "claude"},
                     {"args", QJsonArray{"attach", "bg-1b2c3d4e"}}}));
        // Not a background session, or not listed at all.
        QCOMPARE(Ask("agent.attachCommand",
                     {{"agent", "claude"}, {"session", "fg-4e5f6071"}}),
                 (QJsonObject{{"jsonrpc", "2.0"},
                              {"id", NextId_ - 1},
                              {"result", QJsonValue()}}));
        QCOMPARE(Ask("agent.attachCommand",
                     {{"agent", "claude"}, {"session", "nobody"}})
                     .value("result"),
                 QJsonValue());
    }

    void BadParamsAreRefused_data() {
        QTest::addColumn<QString>("method");
        QTest::addColumn<QJsonObject>("params");
        QTest::newRow("list, another agent")
            << "agent.listSessions" << QJsonObject{{"agent", "codex"}};
        QTest::newRow("list, no agent")
            << "agent.listSessions" << QJsonObject{};
        QTest::newRow("attach, another agent")
            << "agent.attachCommand"
            << QJsonObject{{"agent", "codex"}, {"session", "bg-0a1b2c3d"}};
        QTest::newRow("attach, no session")
            << "agent.attachCommand" << QJsonObject{{"agent", "claude"}};
        QTest::newRow("attach, an empty session")
            << "agent.attachCommand"
            << QJsonObject{{"agent", "claude"}, {"session", ""}};
        QTest::newRow("attach, a session that is no string")
            << "agent.attachCommand"
            << QJsonObject{{"agent", "claude"}, {"session", 1}};
    }

    void BadParamsAreRefused() {
        QFETCH(QString, method);
        QFETCH(QJsonObject, params);
        Put("agents.json", ReadFixture());
        // The answer does not wait for a run.
        Put("hang", "");
        StartAdapter();
        const QJsonObject response = Ask(method, params);
        QCOMPARE(response.value("error").toObject().value("code").toInt(),
                 -32602);
    }

    void AnUnknownMethodIsNotFound() {
        Put("agents.json", ReadFixture());
        StartAdapter();
        QCOMPARE(Ask("agent.start", {{"agent", "claude"}})
                     .value("error")
                     .toObject()
                     .value("code")
                     .toInt(),
                 -32601);
    }

    void ChangesAreSentAfterTheFirstRun() {
        Put("agents.json",
            Agents({Entry("s1", "working"), Entry("s2", "working")}));
        StartAdapter();
        QVERIFY(ListSessions().isArray());
        QVERIFY(WaitForRuns(2));
        // The first run tells nothing: roostd lists what there is.
        QCOMPARE(Notifications().size(), 0);

        Put("agents.json",
            Agents({Entry("s1", "working"), Entry("s2", "needs_input"),
                    Entry("s3", "working")}));
        QVERIFY(WaitForRuns(3));
        const QList<QJsonObject> sent = Notifications();
        QCOMPARE(sent.size(), 2);
        QCOMPARE(sent[0], (QJsonObject{
                              {"jsonrpc", "2.0"},
                              {"method", "session.stateChanged"},
                              {"params", QJsonObject{{"agent", "claude"},
                                                     {"session",
                                                      QJsonObject{
                                                          {"id", "s2"},
                                                          {"state", "blocked"},
                                                          {"title", "s2"},
                                                          {"cwd", ""},
                                                      }}}},
                          }));
        QCOMPARE(sent[1]
                     .value("params")
                     .toObject()
                     .value("session")
                     .toObject()
                     .value("id"),
                 QJsonValue("s3"));
    }

    void AFailedRunFailsRequestsUntilOneSucceeds() {
        Put("stderr", "Error: not logged in\nRun claude login\n");
        Put("exit", "2");
        StartAdapter();
        QCOMPARE(ListSessions(),
                 QJsonValue(QJsonObject{
                     {"code", -32603},
                     {"message",
                      "claude agents exited with code 2: Error: not logged "
                      "in"}}));
        QCOMPARE(
            Ask("agent.attachCommand", {{"agent", "claude"}, {"session", "s1"}})
                .value("error")
                .toObject()
                .value("code")
                .toInt(),
            -32603);

        QVERIFY(QFile::remove(FakePath("exit")));
        QVERIFY(QFile::remove(FakePath("stderr")));
        Put("agents.json", Agents({Entry("s1", "idle")}));
        QVERIFY(WaitForRuns(2));
        QCOMPARE(ListSessions().toArray().size(), 1);
        // The first run that succeeds tells nothing either.
        QCOMPARE(Notifications().size(), 0);
    }

    void AChangeDuringFailedRunsIsSent() {
        Put("agents.json",
            Agents({Entry("s1", "working"), Entry("s2", "working")}));
        StartAdapter();
        QVERIFY(ListSessions().isArray());
        Put("exit", "1");
        QVERIFY(WaitForRuns(2));
        QVERIFY(ListSessions().isObject());
        Put("agents.json",
            Agents({Entry("s1", "done"), Entry("s2", "working")}));
        QVERIFY(QFile::remove(FakePath("exit")));
        QVERIFY(WaitForRuns(2));
        // Compared with the last run that succeeded: s2 is as it was.
        const QList<QJsonObject> sent = Notifications();
        QCOMPARE(sent.size(), 1);
        QCOMPARE(sent[0]
                     .value("params")
                     .toObject()
                     .value("session")
                     .toObject()
                     .value("state"),
                 QJsonValue("done"));
    }

    void ASessionNoLongerListedIsSentAsRemoved() {
        Put("agents.json",
            Agents({Entry("s1", "working"), Entry("s2", "working"),
                    Entry("s3", "working")}));
        StartAdapter();
        QVERIFY(ListSessions().isArray());
        QVERIFY(WaitForRuns(2));
        QCOMPARE(Notifications().size(), 0);

        Put("agents.json", Agents({Entry("s2", "working")}));
        QVERIFY(WaitForRuns(3));
        // In the order of the previous listing.
        QList<QJsonObject> sent = Notifications();
        QCOMPARE(sent.size(), 2);
        QCOMPARE(
            sent[0],
            (QJsonObject{
                {"jsonrpc", "2.0"},
                {"method", "session.removed"},
                {"params", QJsonObject{{"agent", "claude"}, {"session", "s1"}}},
            }));
        QCOMPARE(Removals(sent), (QStringList{"claude/s1", "claude/s3"}));

        // Said once: more runs of the same listing add nothing.
        QVERIFY(WaitForRuns(3));
        QCOMPARE(Notifications().size(), 2);

        // A session that comes back is a change, not a removal.
        Put("agents.json",
            Agents({Entry("s1", "working"), Entry("s2", "working")}));
        QVERIFY(WaitForRuns(3));
        sent = Notifications();
        QCOMPARE(sent.size(), 3);
        QCOMPARE(sent[2].value("method"), QJsonValue("session.stateChanged"));
        QCOMPARE(sent[2]
                     .value("params")
                     .toObject()
                     .value("session")
                     .toObject()
                     .value("id"),
                 QJsonValue("s1"));
    }

    void ARemovalDuringFailedRunsIsSentOnce() {
        Put("agents.json",
            Agents({Entry("s1", "working"), Entry("s2", "working")}));
        StartAdapter();
        QVERIFY(ListSessions().isArray());
        Put("exit", "1");
        QVERIFY(WaitForRuns(2));
        QVERIFY(ListSessions().isObject());
        // A failed run is no empty listing: nothing is removed by it.
        QCOMPARE(Notifications().size(), 0);
        Put("agents.json", Agents({Entry("s2", "working")}));
        QVERIFY(QFile::remove(FakePath("exit")));
        QVERIFY(WaitForRuns(3));
        QCOMPARE(Removals(Notifications()), (QStringList{"claude/s1"}));
        QVERIFY(WaitForRuns(2));
        QCOMPARE(Notifications().size(), 1);
    }

    // One run with a change and a removal: the change goes first.
    void AChangeIsSentBeforeARemovalOfTheSameRun() {
        Put("agents.json",
            Agents({Entry("s1", "working"), Entry("s2", "working")}));
        StartAdapter();
        QVERIFY(ListSessions().isArray());
        QVERIFY(WaitForRuns(2));
        QCOMPARE(Notifications().size(), 0);

        Put("agents.json", Agents({Entry("s1", "done")}));
        QVERIFY(WaitForRuns(3));
        const QList<QJsonObject> sent = Notifications();
        QCOMPARE(sent.size(), 2);
        QCOMPARE(sent[0].value("method"), QJsonValue("session.stateChanged"));
        QCOMPARE(sent[0]
                     .value("params")
                     .toObject()
                     .value("session")
                     .toObject()
                     .value("id"),
                 QJsonValue("s1"));
        QCOMPARE(sent[1].value("method"), QJsonValue("session.removed"));
        QCOMPARE(Removals(sent), (QStringList{"claude/s2"}));
    }

    // An empty list that the run printed is an answer: all of them are gone,
    // in the order of the last listing (not sorted).
    void AnEmptyListingAfterANonEmptyOneRemovesAll() {
        Put("agents.json",
            Agents({Entry("s3", "working"), Entry("s1", "working"),
                    Entry("s2", "working")}));
        StartAdapter();
        QVERIFY(ListSessions().isArray());
        QVERIFY(WaitForRuns(2));
        QCOMPARE(Notifications().size(), 0);

        Put("agents.json", Agents({}));
        QVERIFY(WaitForRuns(3));
        QCOMPARE(Removals(Notifications()),
                 (QStringList{"claude/s3", "claude/s1", "claude/s2"}));
        QVERIFY(WaitForRuns(2));
        QCOMPARE(Notifications().size(), 3);
    }

    void BlankLinesAreSkipped() {
        Put("agents.json", Agents({}));
        StartAdapter();
        Adapter_->write("\n  \n\t\n");
        QCOMPARE(ListSessions(), QJsonValue(QJsonArray{}));
        // Only the answer came back: no error for the blank lines.
        QCOMPARE(Received_.size(), 1);
    }

    void OutputThatIsNoArrayFailsTheRun() {
        Put("agents.json", "Claude Code is updating\n");
        StartAdapter();
        QCOMPARE(ListSessions(),
                 QJsonValue(QJsonObject{
                     {"code", -32603},
                     {"message", "claude agents printed no JSON array"}}));
    }

    void WithoutClaudeTheRunFails() {
        StartAdapter({}, EClaude::None);
        const QString message =
            ListSessions().toObject().value("message").toString();
        QVERIFY2(message.startsWith("cannot run claude agents: "),
                 qPrintable(message));
    }

    void ARunThatHangsIsKilled() {
        Put("hang", "");
        // Long enough for sh to start under load and note its pids.
        StartAdapter({"--timeout-ms", "1500", "--poll-ms", "200"});
        QCOMPARE(ListSessions(),
                 QJsonValue(QJsonObject{
                     {"code", -32603},
                     {"message", "claude agents did not finish in 1500 ms"}}));
        // The first run: what claude started, then claude.
        QFile pidFile(FakePath("pids"));
        QVERIFY(pidFile.open(QIODevice::ReadOnly));
        const QList<QByteArray> pids = pidFile.readAll().split('\n');
        QVERIFY2(pids.size() >= 2, pids.join(' ').constData());
        for (const QByteArray& line : pids.first(2)) {
            const pid_t pid = line.toInt();
            QVERIFY(pid > 0);
            QTRY_VERIFY_WITH_TIMEOUT(::kill(pid, 0) != 0, TIMEOUT_MS);
        }
        // And runs again.
        QVERIFY(QTest::qWaitFor(
            [&] { return Calls().count("start agents --json --all") >= 2; },
            TIMEOUT_MS));
    }

    void AnExitWithoutStderrSaysOnlyTheCode() {
        Put("exit", "3");
        StartAdapter();
        QCOMPARE(ListSessions(),
                 QJsonValue(QJsonObject{
                     {"code", -32603},
                     {"message", "claude agents exited with code 3"}}));
    }

    void ClaudeGetsDefaultSignalHandlers() {
        // The adapter ignores SIGPIPE; claude must not. yes dies of SIGPIPE
        // with 141, and exits with 1 if it is ignored.
        Put("sigpipe", "");
        Put("agents.json", Agents({}));
        StartAdapter();
        QVERIFY(QTest::qWaitFor(
            [&] {
                QFile yes(FakePath("yes"));
                return yes.open(QIODevice::ReadOnly) &&
                       yes.readAll().trimmed() == "141";
            },
            TIMEOUT_MS));
    }

    void RunsOneAtATime() {
        Put("agents.json", Agents({}));
        Put("sleep", "0.1");
        StartAdapter({"--poll-ms", "1"});
        QVERIFY(WaitForRuns(4));
        const QStringList calls = Calls();
        // A run that started has ended before the next one starts.
        for (qsizetype i = 0; i + 1 < calls.size(); ++i) {
            QVERIFY2(calls[i].startsWith("start") !=
                         calls[i + 1].startsWith("start"),
                     qPrintable(calls.join(" | ")));
        }
    }

    void ExitsWhenItsStdinCloses() {
        Put("hang", "");
        StartAdapter();
        Write(Request(1, "agent.listSessions", {{"agent", "claude"}}));
        QVERIFY(QTest::qWaitFor([&] { return QFile::exists(FakePath("pids")); },
                                TIMEOUT_MS));
        Adapter_->closeWriteChannel();
        QVERIFY(Adapter_->waitForFinished(TIMEOUT_MS));
        QCOMPARE(Adapter_->exitStatus(), QProcess::NormalExit);
        QCOMPARE(Adapter_->exitCode(), 0);
        // The request still gets its answer on the way out.
        QCOMPARE(Response(1).value_or(QJsonObject{}).value("error"),
                 QJsonValue(QJsonObject{
                     {"code", -32603},
                     {"message",
                      "the adapter stopped before claude agents answered"}}));
    }

    void ATooLongLineEndsIt() {
        Put("agents.json", Agents({}));
        StartAdapter();
        Adapter_->write(QByteArray(17 * 1024 * 1024, 'x'));
        QVERIFY(Adapter_->waitForFinished(TIMEOUT_MS));
        QCOMPARE(Adapter_->exitCode(), 1);
    }

    void TheRealClaudeIsListed() {
        if (qgetenv("ROOST_REAL_CLAUDE") != "1") {
            QSKIP("set ROOST_REAL_CLAUDE=1 to list the real claude agents");
        }
        QProcess claude;
        claude.start("claude", {"agents", "--json", "--all"});
        QVERIFY(claude.waitForFinished(20'000));
        QSet<QString> expected;
        for (const QJsonValue& entry :
             QJsonDocument::fromJson(claude.readAllStandardOutput()).array()) {
            if (entry.toObject().value("kind") == QJsonValue("background")) {
                expected.insert(entry.toObject().value("id").toString());
            }
        }
        StartAdapter({"--poll-ms", "3000"}, EClaude::Real);
        const QJsonObject response =
            Ask("agent.listSessions", {{"agent", "claude"}});
        QVERIFY2(response.contains("result"),
                 QJsonDocument(response).toJson().constData());
        QSet<QString> listed;
        for (const QJsonValue& session : response.value("result").toArray()) {
            listed.insert(session.toObject().value("id").toString());
        }
        QCOMPARE(listed, expected);
    }
};

QTEST_GUILESS_MAIN(ClaudeAdapterTest)
#include "claude_adapter_test.moc"
