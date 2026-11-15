#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <chrono>
#include <memory>
#include <type_traits>

#include "core/frame.hpp"
#include "core/protocol.hpp"
#include "core/rpc_client.hpp"
#include "roostd/adapter_bridge.hpp"
#include "roostd/backend.hpp"
#include "roostd/plugins.hpp"
#include "roostd/server.hpp"

using namespace roost;
using namespace std::chrono_literals;

namespace {

constexpr int TIMEOUT_MS = 5000;

template <typename TValue>
bool WaitFor(const QFuture<TValue>& future, int timeoutMs = TIMEOUT_MS) {
    if (future.isFinished()) {
        return true;
    }
    QEventLoop loop;
    QFutureWatcher<TValue> watcher;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop,
                     &QEventLoop::quit);
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    watcher.setFuture(future);
    loop.exec();
    return future.isFinished();
}

// The error to show when an expected value is missing. QVERIFY2 computes its
// message before the check, so it must not call error() on a value.
template <typename TExpected>
QString Why(const TExpected& expected) {
    if constexpr (std::is_same_v<typename TExpected::error_type, QString>) {
        return expected ? QString() : expected.error();
    } else {
        return expected ? QString() : expected.error().Message;
    }
}

constexpr int UNAVAILABLE =
    static_cast<int>(protocol::EErrorCode::BackendUnavailable);

BackendOptions Fast() {
    return BackendOptions{
        .FirstRestartDelay = 10ms,
        .MaxRestartDelay = 10ms,
        .MaxRestarts = 5,
        .StableRun = 60s,
        .StopTimeout = 1s,
    };
}

QJsonObject Result(const QJsonValue& value) { return {{"result", value}}; }

QJsonObject Session(const QString& id, const QString& state) {
    return {{"id", id}, {"state", state}};
}

QJsonObject Notification(const QString& method, const QJsonValue& params) {
    return {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
}

}  // namespace

class AgentsTest : public QObject {
    Q_OBJECT

private:
    QString PluginsPath() const { return Dir_->filePath("plugins"); }

    // A plugin whose backend is the fixture and serves these agents.
    void AddPlugin(const QString& id, const QStringList& agents) {
        const QDir directory(PluginsPath() + u'/' + id);
        QVERIFY(directory.mkpath("."));
        QVERIFY(
            QFile::link(FIXTURE_BACKEND_PATH, directory.filePath("backend")));
        QJsonArray declared;
        for (const QString& agent : agents) {
            declared.append(
                QJsonObject{{"id", agent},
                            {"capabilities", QJsonArray{"nativeTuiAttach"}}});
        }
        const QJsonObject manifest{
            {"id", id},
            {"version", "1"},
            {"api", PLUGIN_API},
            {"backend", QJsonObject{{"command", QJsonArray{"backend"}}}},
            {"contributes", QJsonObject{{"agents", declared}}},
        };
        QFile file(directory.filePath("plugin.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(manifest).toJson());
    }

    // What the backend of the plugin answers to agent methods; see
    // tests/helpers/fixture_backend.cpp.
    void SetAnswers(const QString& pluginId, const QJsonObject& answers) {
        QFile file(PluginsPath() + u'/' + pluginId + "/agent.json");
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(answers).toJson());
    }

    bool Start() {
        Server_ = std::make_unique<Server>();
        if (!Server_->Listen(Dir_->filePath("roostd.sock"))) {
            return false;
        }
        Host_ = std::make_unique<PluginHost>(*Server_, Fast());
        Host_->Load({PluginsPath()});
        Client_ = std::make_unique<RpcClient>();
        const QFuture<rpc::Result> hello =
            Client_->Connect(Dir_->filePath("roostd.sock"));
        return WaitFor(hello) && hello.result();
    }

    rpc::Result ListSessions() {
        const QFuture<rpc::Result> future =
            Client_->Request("sessions.list", QJsonObject{});
        if (!WaitFor(future)) {
            return std::unexpected(rpc::Error{.Message = "timed out"});
        }
        return future.result();
    }

    // Has the backend of the plugin send these notifications.
    void Notify(const QString& pluginId, const QJsonArray& messages) {
        const QFuture<rpc::Result> sent = Host_->BackendOf(pluginId)->Request(
            "commands.run",
            QJsonObject{{"command", "fixture.notify"}, {"args", messages}});
        QVERIFY(WaitFor(sent));
        QVERIFY(sent.result());
    }

    std::unique_ptr<QTemporaryDir> Dir_;
    std::unique_ptr<Server> Server_;
    std::unique_ptr<PluginHost> Host_;
    std::unique_ptr<RpcClient> Client_;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
    }

    void cleanup() {
        Client_.reset();
        Host_.reset();
        Server_.reset();
    }

    void ASessionIsReadFromWhatAnAdapterSends() {
        const auto session = SessionFromJson(
            "claude", QJsonObject{{"id", "s1"},
                                  {"state", "blocked"},
                                  {"title", "fix the build"},
                                  {"cwd", "/src/roost"},
                                  {"startedAt", qint64{1790153871692}},
                                  {"pid", 77}});
        QVERIFY2(session, qPrintable(Why(session)));
        QCOMPARE(*session, (AgentSession{
                               .Agent = "claude",
                               .Id = "s1",
                               .Title = "fix the build",
                               .WorkingDirectory = "/src/roost",
                               .State = EAgentState::Blocked,
                               .StartedAt = 1790153871692,
                           }));
    }

    void ASessionNeedsOnlyAnIdAndAState() {
        const auto session = SessionFromJson("claude", Session("s1", "done"));
        QVERIFY2(session, qPrintable(Why(session)));
        QCOMPARE(*session, (AgentSession{.Agent = "claude",
                                         .Id = "s1",
                                         .Title = "",
                                         .WorkingDirectory = "",
                                         .State = EAgentState::Done,
                                         .StartedAt = 0}));
    }

    void EveryStateIsKnown_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<EAgentState>("state");
        QTest::newRow("working") << "working" << EAgentState::Working;
        QTest::newRow("blocked") << "blocked" << EAgentState::Blocked;
        QTest::newRow("idle") << "idle" << EAgentState::Idle;
        QTest::newRow("done") << "done" << EAgentState::Done;
        QTest::newRow("failed") << "failed" << EAgentState::Failed;
    }

    void EveryStateIsKnown() {
        QFETCH(QString, name);
        QFETCH(EAgentState, state);
        const auto session = SessionFromJson("claude", Session("s1", name));
        QVERIFY2(session, qPrintable(Why(session)));
        QCOMPARE(session->State, state);
        QCOMPARE(ToJson(*session).value("state").toString(), name);
    }

    void ASessionGoesToJsonAndBack() {
        const AgentSession session{
            .Agent = "claude",
            .Id = "s1",
            .Title = "t",
            .WorkingDirectory = "/w",
            .State = EAgentState::Working,
            .StartedAt = 5,
        };
        QCOMPARE(ToJson(session), (QJsonObject{{"agent", "claude"},
                                               {"id", "s1"},
                                               {"title", "t"},
                                               {"cwd", "/w"},
                                               {"state", "working"},
                                               {"startedAt", 5}}));
        QCOMPARE(SessionFromJson("claude", ToJson(session)).value(), session);
        AgentSession unknownStart = session;
        unknownStart.StartedAt = 0;
        QVERIFY(!ToJson(unknownStart).contains("startedAt"));
    }

    void BadSessionsAreRefused_data() {
        QTest::addColumn<QJsonValue>("json");
        QTest::addColumn<QString>("error");
        const auto with = [](const QString& key, const QJsonValue& value) {
            QJsonObject session = Session("s1", "idle");
            session.insert(key, value);
            return QJsonValue(session);
        };
        QTest::newRow("array") << QJsonValue(QJsonArray{}) << "not an object";
        QTest::newRow("no id")
            << QJsonValue(QJsonObject{{"state", "idle"}}) << "has no id";
        QTest::newRow("empty id") << with("id", "") << "has no id";
        QTest::newRow("id as number") << with("id", 1) << "has no id";
        QTest::newRow("no state")
            << QJsonValue(QJsonObject{{"id", "s1"}}) << "has no state";
        QTest::newRow("unknown state")
            << with("state", "waiting") << "has no state";
        QTest::newRow("state in capitals")
            << with("state", "Idle") << "has no state";
        QTest::newRow("title as number")
            << with("title", 1) << "must be strings";
        QTest::newRow("cwd as null")
            << with("cwd", QJsonValue()) << "must be strings";
        QTest::newRow("startedAt as string")
            << with("startedAt", "1790153871692") << "startedAt";
        QTest::newRow("startedAt zero") << with("startedAt", 0) << "startedAt";
        QTest::newRow("startedAt negative")
            << with("startedAt", -5) << "startedAt";
        QTest::newRow("startedAt fractional")
            << with("startedAt", 1.5) << "startedAt";
    }

    void BadSessionsAreRefused() {
        QFETCH(QJsonValue, json);
        QFETCH(QString, error);
        const auto session = SessionFromJson("claude", json);
        QVERIFY(!session);
        QVERIFY2(session.error().contains(error), qPrintable(session.error()));
    }

    void AnAttachCommandIsReadFromWhatAnAdapterSends() {
        const auto spec = AttachCommandFromJson(
            QJsonObject{{"program", "claude"},
                        {"args", QJsonArray{"attach", "s1"}},
                        {"cwd", "/src/roost"},
                        {"env", QJsonObject{{"A", "1"}, {"B", ""}}}});
        QVERIFY2(spec, qPrintable(Why(spec)));
        QVERIFY(spec->has_value());
        const PtySpec& command = **spec;
        QCOMPARE(command.Program, QString("claude"));
        QCOMPARE(command.Arguments, (QStringList{"attach", "s1"}));
        QCOMPARE(command.WorkingDirectory, QString("/src/roost"));
        QCOMPARE(command.Environment, (QStringList{"A=1", "B="}));
    }

    void AnAttachCommandNeedsOnlyAProgram() {
        const auto spec =
            AttachCommandFromJson(QJsonObject{{"program", "/bin/sh"}});
        QVERIFY2(spec, qPrintable(Why(spec)));
        QVERIFY(spec->has_value());
        QCOMPARE((*spec)->Program, QString("/bin/sh"));
        QVERIFY((*spec)->Arguments.isEmpty());
        QVERIFY((*spec)->WorkingDirectory.isEmpty());
        QVERIFY((*spec)->Environment.isEmpty());
    }

    void NullMeansNoAttaching() {
        const auto spec = AttachCommandFromJson(QJsonValue());
        QVERIFY2(spec, qPrintable(Why(spec)));
        QVERIFY(!spec->has_value());
    }

    void BadAttachCommandsAreRefused_data() {
        QTest::addColumn<QJsonValue>("json");
        QTest::addColumn<QString>("error");
        const auto with = [](const QString& key, const QJsonValue& value) {
            QJsonObject command{{"program", "claude"}};
            command.insert(key, value);
            return QJsonValue(command);
        };
        QTest::newRow("string") << QJsonValue("claude") << "is null or";
        QTest::newRow("absent")
            << QJsonValue(QJsonValue::Undefined) << "is null or";
        QTest::newRow("no program")
            << QJsonValue(QJsonObject{{"args", QJsonArray{}}}) << "is null or";
        QTest::newRow("empty program") << with("program", "") << "is null or";
        QTest::newRow("program as array")
            << with("program", QJsonArray{"claude"}) << "is null or";
        QTest::newRow("args as string")
            << with("args", "attach s1") << "args must be an array";
        QTest::newRow("arg as number")
            << with("args", QJsonArray{"attach", 1}) << "args must be strings";
        QTest::newRow("cwd as number")
            << with("cwd", 1) << "cwd must be a string";
        QTest::newRow("env as array")
            << with("env", QJsonArray{"A=1"}) << "env must be an object";
        QTest::newRow("env value as number")
            << with("env", QJsonObject{{"A", 1}}) << "env must map";
        QTest::newRow("env name with =")
            << with("env", QJsonObject{{"A=B", "1"}}) << "env must map";
        QTest::newRow("env name empty")
            << with("env", QJsonObject{{"", "1"}}) << "env must map";
    }

    void BadAttachCommandsAreRefused() {
        QFETCH(QJsonValue, json);
        QFETCH(QString, error);
        const auto spec = AttachCommandFromJson(json);
        QVERIFY(!spec);
        QVERIFY2(spec.error().contains(error), qPrintable(spec.error()));
    }

    void ListsTheSessionsOfEveryAgent() {
        AddPlugin("one", {"alpha", "beta"});
        AddPlugin("two", {"gamma"});
        SetAnswers("one", {{"agent.listSessions",
                            QJsonObject{
                                {"alpha", Result(QJsonArray{
                                              Session("a1", "working"),
                                              Session("a2", "blocked"),
                                          })},
                                {"beta", Result(QJsonArray{})},
                            }}});
        SetAnswers("two", {{"agent.listSessions",
                            QJsonObject{{"gamma", Result(QJsonArray{Session(
                                                      "g1", "done")})}}}});
        QVERIFY(Start());
        const rpc::Result listed = ListSessions();
        QVERIFY2(listed, qPrintable(Why(listed)));
        QCOMPARE(*listed, QJsonValue(QJsonObject{
                              {"sessions",
                               QJsonArray{
                                   QJsonObject{{"agent", "alpha"},
                                               {"id", "a1"},
                                               {"title", ""},
                                               {"cwd", ""},
                                               {"state", "working"}},
                                   QJsonObject{{"agent", "alpha"},
                                               {"id", "a2"},
                                               {"title", ""},
                                               {"cwd", ""},
                                               {"state", "blocked"}},
                                   QJsonObject{{"agent", "gamma"},
                                               {"id", "g1"},
                                               {"title", ""},
                                               {"cwd", ""},
                                               {"state", "done"}},
                               }},
                              {"unavailable", QJsonArray{}},
                          }));
    }

    void WithoutAgentsThereAreNoSessions() {
        AddPlugin("one", {});
        QVERIFY(Start());
        const rpc::Result listed = ListSessions();
        QVERIFY2(listed, qPrintable(Why(listed)));
        QCOMPARE(*listed, QJsonValue(QJsonObject{
                              {"sessions", QJsonArray{}},
                              {"unavailable", QJsonArray{}},
                          }));
    }

    void AFailingAdapterIsUnavailable() {
        AddPlugin("one", {"alpha", "beta", "delta"});
        AddPlugin("two", {"gamma"});
        SetAnswers(
            "one",
            {{"agent.listSessions",
              QJsonObject{
                  {"alpha",
                   QJsonObject{{"error",
                                QJsonObject{{"code", 1},
                                            {"message", "claude not found"}}}}},
                  {"beta", Result(QJsonObject{})},
                  {"delta", Result(QJsonArray{Session("d1", "idle"),
                                              Session("d1", "done")})},
              }}});
        SetAnswers("two",
                   {{"agent.listSessions", QJsonObject{{"gamma", "crash"}}}});
        // The other agents are listed; this one is fine.
        AddPlugin("three", {"epsilon"});
        SetAnswers("three", {{"agent.listSessions",
                              QJsonObject{{"epsilon", Result(QJsonArray{Session(
                                                          "e1", "idle")})}}}});
        QVERIFY(Start());
        const rpc::Result listed = ListSessions();
        QVERIFY2(listed, qPrintable(Why(listed)));
        const QJsonArray sessions =
            listed->toObject().value("sessions").toArray();
        QCOMPARE(sessions.size(), 1);
        QCOMPARE(sessions[0].toObject().value("id").toString(), QString("e1"));
        const QJsonArray unavailable =
            listed->toObject().value("unavailable").toArray();
        QCOMPARE(unavailable.size(), 4);
        const auto reason = [&](qsizetype i) {
            return unavailable[i].toObject().value("reason").toString();
        };
        QCOMPARE(unavailable[0].toObject().value("agent").toString(),
                 QString("alpha"));
        QCOMPARE(reason(0), QString("claude not found"));
        QCOMPARE(unavailable[1].toObject().value("agent").toString(),
                 QString("beta"));
        QCOMPARE(reason(1),
                 QString("the adapter of beta answered agent.listSessions "
                         "badly: not an array"));
        QCOMPARE(unavailable[2].toObject().value("agent").toString(),
                 QString("delta"));
        QVERIFY2(reason(2).endsWith("session d1 is listed twice"),
                 qPrintable(reason(2)));
        QCOMPARE(unavailable[3].toObject().value("agent").toString(),
                 QString("gamma"));
        QVERIFY2(reason(3).contains("exited with code 3"),
                 qPrintable(reason(3)));
    }

    void AnAgentStaysWithTheFirstPluginToServeIt() {
        AddPlugin("one", {"claude"});
        AddPlugin("two", {"claude", "codex"});
        SetAnswers("one", {{"agent.listSessions",
                            QJsonObject{{"claude", Result(QJsonArray{Session(
                                                       "mine", "idle")})}}}});
        SetAnswers(
            "two",
            {{"agent.listSessions",
              QJsonObject{
                  {"claude", Result(QJsonArray{Session("theirs", "idle")})},
                  {"codex", Result(QJsonArray{})},
              }}});
        QVERIFY(Start());
        QVERIFY(Host_->BridgeOf("claude"));
        QVERIFY(Host_->BridgeOf("codex"));
        QVERIFY(!Host_->BridgeOf("gemini"));
        QCOMPARE(Host_->BridgeOf("claude")->Capabilities(),
                 QStringList{"nativeTuiAttach"});
        const rpc::Result listed = ListSessions();
        QVERIFY2(listed, qPrintable(Why(listed)));
        const QJsonArray sessions =
            listed->toObject().value("sessions").toArray();
        QCOMPARE(sessions.size(), 1);
        QCOMPARE(sessions[0].toObject().value("id").toString(),
                 QString("mine"));
    }

    void AnAgentDeclaredTwiceGetsOneBridge() {
        AddPlugin("one", {"claude", "claude"});
        SetAnswers("one", {{"agent.listSessions",
                            QJsonObject{{"claude", Result(QJsonArray{Session(
                                                       "s1", "idle")})}}}});
        QVERIFY(Start());
        const rpc::Result listed = ListSessions();
        QVERIFY2(listed, qPrintable(Why(listed)));
        QCOMPARE(listed->toObject().value("sessions").toArray().size(), 1);
    }

    void TheListFollowsTheLoadOrder() {
        // Plugins load by directory name; agent names sort the other way.
        AddPlugin("a", {"zeta"});
        AddPlugin("b", {"alpha"});
        AddPlugin("c", {"omega"});
        AddPlugin("d", {"beta"});
        SetAnswers(
            "a", {{"agent.listSessions",
                   QJsonObject{
                       {"zeta", Result(QJsonArray{Session("z1", "idle")})}}}});
        SetAnswers(
            "b", {{"agent.listSessions",
                   QJsonObject{
                       {"alpha", Result(QJsonArray{Session("a1", "idle")})}}}});
        SetAnswers("c", {{"agent.listSessions",
                          QJsonObject{{"omega", Result(QJsonObject{})}}}});
        SetAnswers("d", {{"agent.listSessions",
                          QJsonObject{{"beta", Result(QJsonValue())}}}});
        QVERIFY(Start());
        const rpc::Result listed = ListSessions();
        QVERIFY2(listed, qPrintable(Why(listed)));
        const QJsonArray sessions =
            listed->toObject().value("sessions").toArray();
        QCOMPARE(sessions.size(), 2);
        QCOMPARE(sessions[0].toObject().value("id").toString(), QString("z1"));
        QCOMPARE(sessions[1].toObject().value("id").toString(), QString("a1"));
        const QJsonArray unavailable =
            listed->toObject().value("unavailable").toArray();
        QCOMPARE(unavailable.size(), 2);
        QCOMPARE(unavailable[0].toObject().value("agent").toString(),
                 QString("omega"));
        QCOMPARE(unavailable[1].toObject().value("agent").toString(),
                 QString("beta"));
    }

    void AnErrorWithoutAMessageGivesItsCode() {
        AddPlugin("one", {"claude"});
        SetAnswers("one",
                   {{"agent.listSessions",
                     QJsonObject{
                         {"claude",
                          QJsonObject{{"error", QJsonObject{{"code", 7}}}}}}}});
        QVERIFY(Start());
        const rpc::Result listed = ListSessions();
        QVERIFY2(listed, qPrintable(Why(listed)));
        QCOMPARE(listed->toObject().value("unavailable"),
                 QJsonValue(QJsonArray{
                     QJsonObject{{"agent", "claude"}, {"reason", "error 7"}}}));
    }

    void AListTooBigForAFrameIsRefused() {
        AddPlugin("one", {"alpha", "beta"});
        // Each answer fits a line from the backend; the two together do not
        // fit a frame.
        QJsonArray sessions;
        for (int i = 0; i < 9; ++i) {
            QJsonObject session = Session(QString::number(i), "idle");
            session.insert("title", QString(1024 * 1024, u'x'));
            sessions.append(session);
        }
        SetAnswers("one", {{"agent.listSessions",
                            QJsonObject{{"alpha", Result(sessions)},
                                        {"beta", Result(sessions)}}}});
        QVERIFY(Start());
        const rpc::Result listed = ListSessions();
        QVERIFY(!listed);
        QCOMPARE(listed.error().Code,
                 static_cast<int>(rpc::EErrorCode::InternalError));
        QCOMPARE(listed.error().Message,
                 QString("the session list does not fit a frame"));
    }

    void AdaptersAreAskedAtOnce() {
        AddPlugin("one", {"alpha"});
        AddPlugin("two", {"beta"});
        SetAnswers("one",
                   {{"agent.listSessions", QJsonObject{{"alpha", "hang"}}}});
        SetAnswers("two",
                   {{"agent.listSessions", QJsonObject{{"beta", "crash"}}}});
        QVERIFY(Start());
        QSignalSpy crashed(Host_->BackendOf("two"), &Backend::Crashed);
        const QFuture<rpc::Result> future =
            Client_->Request("sessions.list", QJsonObject{});
        // beta is asked although alpha, loaded first, has not answered.
        QTRY_VERIFY(!crashed.isEmpty());
        QVERIFY(!future.isFinished());
    }

    void TheAttachCommandComesFromTheAdapter() {
        AddPlugin("one", {"claude"});
        SetAnswers(
            "one",
            {{"agent.attachCommand",
              QJsonObject{
                  {"claude/s1",
                   Result(QJsonObject{{"program", "claude"},
                                      {"args", QJsonArray{"attach", "s1"}}})},
                  {"claude", Result(QJsonValue())},
              }}});
        QVERIFY(Start());
        AdapterBridge* bridge = Host_->BridgeOf("claude");
        QVERIFY(bridge);

        const auto attach = bridge->AttachCommand("s1");
        QVERIFY(WaitFor(attach));
        const auto spec = attach.result();
        QVERIFY2(spec, qPrintable(Why(spec)));
        QVERIFY(spec->has_value());
        QCOMPARE((*spec)->Program, QString("claude"));
        QCOMPARE((*spec)->Arguments, (QStringList{"attach", "s1"}));

        const auto other = bridge->AttachCommand("s2");
        QVERIFY(WaitFor(other));
        QVERIFY2(other.result(), qPrintable(Why(other.result())));
        QVERIFY(!other.result()->has_value());
    }

    void ABadAttachCommandFails() {
        AddPlugin("one", {"claude"});
        SetAnswers(
            "one",
            {{"agent.attachCommand",
              QJsonObject{{"claude", Result(QJsonObject{{"args", 1}})}}}});
        QVERIFY(Start());
        const auto attach = Host_->BridgeOf("claude")->AttachCommand("s1");
        QVERIFY(WaitFor(attach));
        QVERIFY(!attach.result());
        QCOMPARE(attach.result().error().Code,
                 static_cast<int>(rpc::EErrorCode::InternalError));
        QVERIFY2(attach.result().error().Message.startsWith(
                     "the adapter of claude answered agent.attachCommand "
                     "badly: "),
                 qPrintable(attach.result().error().Message));
    }

    void AStateChangeIsPublished() {
        AddPlugin("one", {"claude"});
        AddPlugin("two", {"codex"});
        QVERIFY(Start());
        QList<QJsonValue> events;
        QVERIFY(Server_->Bus().Subscribe(
            "session.*", [&](const QString& name, const QJsonValue& payload) {
                events.append(
                    QJsonObject{{"name", name}, {"payload", payload}});
            }));
        QTest::ignoreMessage(QtWarningMsg,
                             "roostd: the backend of one told the state of "
                             "agent \"codex\", which it does not serve");
        QTest::ignoreMessage(QtWarningMsg,
                             "roostd: the backend of one told the state of "
                             "agent \"\", which it does not serve");
        QTest::ignoreMessage(QtWarningMsg,
                             "roostd: the backend of one told a bad session "
                             "state: session s1 has no state of working, "
                             "blocked, idle, done or failed");
        Notify(
            "one",
            {
                Notification(
                    "session.stateChanged",
                    QJsonObject{{"agent", "claude"},
                                {"session", QJsonObject{{"id", "s1"},
                                                        {"state", "blocked"},
                                                        {"title", "t"}}}}),
                // Not its agent.
                Notification("session.stateChanged",
                             QJsonObject{{"agent", "codex"},
                                         {"session", Session("c1", "done")}}),
                Notification("session.stateChanged",
                             QJsonObject{{"session", Session("s1", "done")}}),
                Notification("session.stateChanged",
                             QJsonObject{{"agent", "claude"},
                                         {"session", Session("s1", "gone")}}),
                // Only session.stateChanged tells a state.
                Notification("session.titleChanged",
                             QJsonObject{{"agent", "claude"},
                                         {"session", Session("s1", "done")}}),
            });
        QTest::ignoreMessage(QtWarningMsg,
                             "roostd: the backend of one told a session "
                             "state too big for a frame");
        // The line from the backend fits; the event, a little longer, does
        // not.
        const QJsonObject huge{
            {"agent", "claude"},
            {"session",
             QJsonObject{{"id", "s2"},
                         {"state", "idle"},
                         {"title", QString(MAX_FRAME_LENGTH - 140, u'x')}}}};
        QVERIFY(QJsonDocument(Notification("session.stateChanged", huge))
                    .toJson(QJsonDocument::Compact)
                    .size() < Backend::MAX_LINE_LENGTH);
        Notify("one", {Notification("session.stateChanged", huge)});
        QCOMPARE(events.size(), 1);
        QCOMPARE(events[0], QJsonValue(QJsonObject{
                                {"name", "session.stateChanged"},
                                {"payload", QJsonObject{{"agent", "claude"},
                                                        {"id", "s1"},
                                                        {"title", "t"},
                                                        {"cwd", ""},
                                                        {"state", "blocked"}}},
                            }));
    }

    void AHangingAdapterHoldsTheListUntilItsBackendStops() {
        AddPlugin("one", {"claude"});
        SetAnswers("one",
                   {{"agent.listSessions", QJsonObject{{"claude", "hang"}}}});
        QVERIFY(Start());
        const QFuture<rpc::Result> future =
            Client_->Request("sessions.list", QJsonObject{});
        QVERIFY(!WaitFor(future, 200));
        // A backend stopped while the server runs on, as when plugins are
        // unloaded, ends the wait. roostd stopping drops the connection
        // instead (design.md §4).
        Host_.reset();
        QVERIFY(WaitFor(future));
        const rpc::Result listed = future.result();
        QVERIFY2(listed, qPrintable(Why(listed)));
        const QJsonArray unavailable =
            listed->toObject().value("unavailable").toArray();
        QCOMPARE(unavailable.size(), 1);
        QCOMPARE(unavailable[0].toObject().value("reason").toString(),
                 QString("roostd stopped the backend of one"));
    }

    void WithoutTheHostThereIsNoList() {
        AddPlugin("one", {"claude"});
        QVERIFY(Start());
        Host_.reset();
        const rpc::Result listed = ListSessions();
        QVERIFY(!listed);
        QCOMPARE(listed.error().Code, UNAVAILABLE);
    }
};

QTEST_GUILESS_MAIN(AgentsTest)
#include "agents_test.moc"
