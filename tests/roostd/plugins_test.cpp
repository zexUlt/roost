#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <signal.h>

#include <cerrno>
#include <chrono>
#include <memory>

#include "core/protocol.hpp"
#include "core/rpc_client.hpp"
#include "roostd/plugins.hpp"
#include "roostd/server.hpp"

using namespace roost;
using namespace std::chrono_literals;

namespace {

constexpr int TIMEOUT_MS = 5000;

template <typename TValue>
bool WaitFor(const QFuture<TValue>& future) {
    if (future.isFinished()) {
        return true;
    }
    QEventLoop loop;
    QFutureWatcher<TValue> watcher;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop,
                     &QEventLoop::quit);
    QTimer::singleShot(TIMEOUT_MS, &loop, &QEventLoop::quit);
    watcher.setFuture(future);
    loop.exec();
    return future.isFinished();
}

int CodeOf(const rpc::Result& result) {
    return result ? 0 : result.error().Code;
}

QString MessageOf(const rpc::Result& result) {
    return result ? QString() : result.error().Message;
}

constexpr int UNAVAILABLE =
    static_cast<int>(protocol::EErrorCode::BackendUnavailable);
constexpr int INVALID_PARAMS = static_cast<int>(rpc::EErrorCode::InvalidParams);

BackendOptions Fast() {
    return BackendOptions{
        .FirstRestartDelay = 10ms,
        .MaxRestartDelay = 10ms,
        .MaxRestarts = 5,
        .StableRun = 60s,
        .StopTimeout = 1s,
    };
}

bool IsAlive(qint64 pid) {
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
}

}  // namespace

class PluginsTest : public QObject {
    Q_OBJECT

private:
    QString PluginsPath() const { return Dir_->filePath("plugins"); }

    // A plugin whose backend is the fixture, run with these arguments.
    void AddPlugin(const QString& id, const QStringList& commands,
                   const QStringList& arguments = {}, bool backend = true) {
        const QDir directory(PluginsPath() + u'/' + id);
        QVERIFY(directory.mkpath("."));
        QJsonArray declared;
        for (const QString& command : commands) {
            declared.append(QJsonObject{{"id", command}, {"title", command}});
        }
        QJsonObject manifest{
            {"id", id},
            {"version", "1"},
            {"api", PLUGIN_API},
            {"contributes", QJsonObject{{"commands", declared}}},
        };
        if (backend) {
            QVERIFY(QFile::link(FIXTURE_BACKEND_PATH,
                                directory.filePath("backend")));
            manifest.insert("backend",
                            QJsonObject{{"command", QJsonArray::fromStringList(
                                                        QStringList{"backend"} +
                                                        arguments)}});
        }
        QFile file(directory.filePath("plugin.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(manifest).toJson());
    }

    // The server and the host, loaded from PluginsPath().
    bool Start(BackendOptions options = Fast()) {
        Server_ = std::make_unique<Server>();
        if (!Server_->Listen(Dir_->filePath("roostd.sock"))) {
            return false;
        }
        Host_ = std::make_unique<PluginHost>(*Server_, options);
        Host_->Load({PluginsPath()});
        Client_ = std::make_unique<RpcClient>();
        const QFuture<rpc::Result> hello =
            Client_->Connect(Dir_->filePath("roostd.sock"));
        return WaitFor(hello) && hello.result();
    }

    rpc::Result Run(const QJsonObject& params) {
        const QFuture<rpc::Result> future =
            Client_->Request("commands.run", params);
        if (!WaitFor(future)) {
            return std::unexpected(rpc::Error{.Message = "timed out"});
        }
        return future.result();
    }

    rpc::Result Run(const QString& command,
                    const QJsonValue& args = QJsonValue::Undefined) {
        QJsonObject params{{"command", command}};
        if (!args.isUndefined()) {
            params.insert("args", args);
        }
        return Run(params);
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

    void RunsACommandOfAPlugin() {
        AddPlugin("fixture", {"fixture.echo"});
        QVERIFY(Start());
        const rpc::Result result =
            Run("fixture.echo", QJsonObject{{"text", "hi"}});
        QVERIFY2(result, qPrintable(MessageOf(result)));
        QCOMPARE(*result, QJsonValue(QJsonObject{{"text", "hi"}}));
    }

    void ArgsMayBeLeftOut() {
        AddPlugin("fixture", {"fixture.echo"});
        QVERIFY(Start());
        const rpc::Result result = Run("fixture.echo");
        QVERIFY2(result, qPrintable(MessageOf(result)));
        QCOMPARE(*result, QJsonValue());
    }

    void CommandsGoToTheirPlugin() {
        AddPlugin("one", {"fixture.echo"});
        AddPlugin("two", {"fixture.pid"});
        QVERIFY(Start());
        const rpc::Result pid = Run("fixture.pid");
        QVERIFY2(pid, qPrintable(MessageOf(pid)));
        QCOMPARE(pid->toInteger(), Host_->BackendOf("two")->ProcessId());
        QVERIFY(pid->toInteger() != Host_->BackendOf("one")->ProcessId());
    }

    void UnknownCommandIsInvalid() {
        AddPlugin("fixture", {"fixture.echo"});
        QVERIFY(Start());
        const rpc::Result result = Run("fixture.pid");
        QCOMPARE(CodeOf(result), INVALID_PARAMS);
        QCOMPARE(MessageOf(result), "no plugin has command fixture.pid");
    }

    void BadParamsAreInvalid() {
        AddPlugin("fixture", {"fixture.echo"});
        QVERIFY(Start());
        for (const QJsonObject& params :
             {QJsonObject{{"command", 1}}, QJsonObject{}}) {
            const rpc::Result result = Run(params);
            QCOMPARE(CodeOf(result), INVALID_PARAMS);
            QVERIFY2(MessageOf(result).startsWith("commands.run takes"),
                     qPrintable(MessageOf(result)));
        }
    }

    void PluginWithoutABackendIsUnavailable() {
        AddPlugin("ui", {"ui.open"}, {}, false);
        QVERIFY(Start());
        QVERIFY(!Host_->BackendOf("ui"));
        const rpc::Result result = Run("ui.open");
        QCOMPARE(CodeOf(result), UNAVAILABLE);
        QCOMPARE(MessageOf(result), "plugin ui has no backend");
    }

    void CrashIsPublished() {
        AddPlugin("fixture", {"fixture.crash", "fixture.echo"});
        QVERIFY(Start());
        QList<QJsonValue> events;
        QVERIFY(Server_->Bus().Subscribe(
            "plugin.*", [&](const QString& name, const QJsonValue& payload) {
                events.append(
                    QJsonObject{{"name", name}, {"payload", payload}});
            }));

        QCOMPARE(CodeOf(Run("fixture.crash")), UNAVAILABLE);
        QCOMPARE(events.size(), 1);
        QCOMPARE(events[0],
                 QJsonValue(QJsonObject{
                     {"name", "plugin.backendCrashed"},
                     {"payload", QJsonObject{{"plugin", "fixture"},
                                             {"reason", "exited with code 3"},
                                             {"restartInMs", 10}}},
                 }));
        // And it is back.
        QTRY_VERIFY_WITH_TIMEOUT(Run("fixture.echo").has_value(), TIMEOUT_MS);
    }

    void ARestartOfABackendWithoutAgentsIsNotAnnounced() {
        AddPlugin("fixture", {"fixture.crash", "fixture.echo"});
        QVERIFY(Start());
        QList<QString> events;
        QVERIFY(Server_->Bus().Subscribe(
            "session.*", [&](const QString& name, const QJsonValue&) {
                events.append(name);
            }));
        QCOMPARE(CodeOf(Run("fixture.crash")), UNAVAILABLE);
        QTRY_VERIFY_WITH_TIMEOUT(Run("fixture.echo").has_value(), TIMEOUT_MS);
        QTest::qWait(50);
        QVERIFY2(events.isEmpty(), qPrintable(events.join(", ")));
    }

    void GivingUpIsPublished() {
        AddPlugin("fixture", {"fixture.echo"}, {"--crash-on-start"});
        BackendOptions options = Fast();
        options.MaxRestarts = 0;
        QTest::ignoreMessage(QtWarningMsg,
                             "roostd: gave up on the backend of fixture: "
                             "exited with code 3");
        QList<QJsonValue> events;
        Server server;
        PluginHost host(server, options);
        QVERIFY(server.Bus().Subscribe(
            "plugin.backendGaveUp",
            [&](const QString&, const QJsonValue& payload) {
                events.append(payload);
            }));
        host.Load({PluginsPath()});
        QTRY_COMPARE_WITH_TIMEOUT(events.size(), 1, TIMEOUT_MS);
        QCOMPARE(events[0], QJsonValue(QJsonObject{
                                {"plugin", "fixture"},
                                {"reason", "exited with code 3"},
                            }));
    }

    void BrokenPluginIsAProblem() {
        AddPlugin("good", {"good.run"}, {}, false);
        QVERIFY(QDir(PluginsPath()).mkpath("bad"));
        QFile broken(PluginsPath() + "/bad/plugin.json");
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("{");
        broken.close();

        Server server;
        PluginHost host(server, Fast());
        host.Load({PluginsPath()});
        QCOMPARE(host.Registry().Plugins().size(), size_t{1});
        QCOMPARE(host.Registry().Problems().size(), 1);
        QVERIFY2(host.Registry().Problems().front().startsWith(
                     PluginsPath() + "/bad/plugin.json: "),
                 qPrintable(host.Registry().Problems().front()));
    }

    void LoadsOnce() {
        AddPlugin("fixture", {"fixture.echo"});
        Server server;
        PluginHost host(server, Fast());
        host.Load({PluginsPath()});
        const Backend* backend = host.BackendOf("fixture");
        host.Load({PluginsPath()});
        QCOMPARE(host.Registry().Plugins().size(), size_t{1});
        QVERIFY(host.Registry().Problems().isEmpty());
        QCOMPARE(host.BackendOf("fixture"), backend);
    }

    void BackendsStopWithTheHost() {
        AddPlugin("fixture", {"fixture.pid"});
        QVERIFY(Start());
        const qint64 pid = Run("fixture.pid")->toInteger();
        QVERIFY(IsAlive(pid));

        Host_.reset();
        QVERIFY(!IsAlive(pid));
        const rpc::Result result = Run("fixture.pid");
        QCOMPARE(CodeOf(result), UNAVAILABLE);
        QCOMPARE(MessageOf(result), "plugins are not loaded");
    }
};

QTEST_GUILESS_MAIN(PluginsTest)
#include "plugins_test.moc"
