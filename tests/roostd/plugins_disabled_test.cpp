// plugins.disabled of the settings document: the plugins roostd leaves out at
// its start, read from a settings.json on disk as roostd reads it.

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

#include <memory>

#include "core/rpc_client.hpp"
#include "core/store.hpp"
#include "roostd/plugins.hpp"
#include "roostd/server.hpp"

using namespace roost;

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

}  // namespace

class PluginsDisabledTest : public QObject {
    Q_OBJECT

private:
    QString PluginsPath() const { return Dir_->filePath("plugins"); }
    QString ConfigPath() const { return Dir_->filePath("config"); }

    void AddPlugin(const QString& id) {
        const QDir directory(PluginsPath() + u'/' + id);
        QVERIFY(directory.mkpath("."));
        QFile file(directory.filePath("plugin.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject{{"id", id},
                                             {"version", "1"},
                                             {"api", PLUGIN_API},
                                             {"contributes", QJsonObject{}}})
                       .toJson());
    }

    // The text of settings.json, as written by hand or by another version.
    void WriteSettings(const QByteArray& text) {
        QVERIFY(QDir().mkpath(ConfigPath()));
        QFile file(ConfigPath() + "/settings.json");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(text);
    }

    // The host of the plugins directory with what the store says is disabled,
    // as roostd's main does it.
    bool Start() {
        Server_ = std::make_unique<Server>();
        if (!Server_->Listen(Dir_->filePath("roostd.sock"))) {
            return false;
        }
        Host_ = std::make_unique<PluginHost>(*Server_);
        Host_->Load({PluginsPath()}, ReadDisabledPlugins(Store(ConfigPath())));
        Client_ = std::make_unique<RpcClient>();
        const QFuture<rpc::Result> hello =
            Client_->Connect(Dir_->filePath("roostd.sock"));
        return WaitFor(hello) && hello.result();
    }

    // The plugins.list answer: the ids in load order and the problems.
    QJsonObject List() {
        const QFuture<rpc::Result> future =
            Client_->Request("plugins.list", QJsonValue::Undefined);
        if (!WaitFor(future) || !future.result()) {
            return {};
        }
        return future.result()->toObject();
    }

    static QStringList IdsOf(const QJsonObject& answer) {
        QStringList ids;
        for (const QJsonValue& plugin : answer.value("plugins").toArray()) {
            ids.append(plugin.toObject().value("id").toString());
        }
        return ids;
    }

    static QStringList ProblemsOf(const QJsonObject& answer) {
        QStringList problems;
        for (const QJsonValue& problem : answer.value("problems").toArray()) {
            problems.append(problem.toString());
        }
        return problems;
    }

    std::unique_ptr<QTemporaryDir> Dir_;
    std::unique_ptr<Server> Server_;
    std::unique_ptr<PluginHost> Host_;
    std::unique_ptr<RpcClient> Client_;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
        AddPlugin("a");
        AddPlugin("b");
        AddPlugin("c");
    }

    void cleanup() {
        Client_.reset();
        Host_.reset();
        Server_.reset();
    }

    void NoSettingsLoadsEverything() {
        QVERIFY(Start());
        const QJsonObject answer = List();
        QCOMPARE(IdsOf(answer), (QStringList{"a", "b", "c"}));
        QVERIFY(ProblemsOf(answer).isEmpty());
    }

    void ListedIdsAreSkippedAndTheRestKeepsItsOrder() {
        WriteSettings(R"({"plugins": {"disabled": ["b", "a"]}})");
        QVERIFY(Start());
        const QJsonObject answer = List();
        QCOMPARE(IdsOf(answer), (QStringList{"c"}));
        QVERIFY(ProblemsOf(answer).isEmpty());
    }

    void AnIdThatIsNotInstalledIsNoProblem() {
        WriteSettings(R"({"plugins": {"disabled": ["nowhere", "c"]}})");
        QVERIFY(Start());
        const QJsonObject answer = List();
        QCOMPARE(IdsOf(answer), (QStringList{"a", "b"}));
        QVERIFY(ProblemsOf(answer).isEmpty());
    }

    void AnEmptyListAndOtherSettingsDisableNothing() {
        WriteSettings(
            R"({"theme": "dark", "plugins": {"disabled": [], "other": 1}})");
        QVERIFY(Start());
        const QJsonObject answer = List();
        QCOMPARE(IdsOf(answer), (QStringList{"a", "b", "c"}));
        QVERIFY(ProblemsOf(answer).isEmpty());
    }

    void AMalformedValueIsAProblemAndDisablesNothing_data() {
        QTest::addColumn<QByteArray>("settings");
        QTest::newRow("a string")
            << QByteArray(R"({"plugins": {"disabled": "a"}})");
        QTest::newRow("an object")
            << QByteArray(R"({"plugins": {"disabled": {"a": true}}})");
        QTest::newRow("a number in the array")
            << QByteArray(R"({"plugins": {"disabled": ["a", 1]}})");
        QTest::newRow("a null in the array")
            << QByteArray(R"({"plugins": {"disabled": ["a", null]}})");
        QTest::newRow("plugins is not an object")
            << QByteArray(R"({"plugins": ["a"]})");
        QTest::newRow("settings.json does not parse")
            << QByteArray(R"({"plugins": {"disabled": ["a")");
    }

    void AMalformedValueIsAProblemAndDisablesNothing() {
        QFETCH(QByteArray, settings);
        WriteSettings(settings);
        QVERIFY(Start());
        const QJsonObject answer = List();
        QCOMPARE(IdsOf(answer), (QStringList{"a", "b", "c"}));
        const QStringList problems = ProblemsOf(answer);
        QCOMPARE(problems.size(), 1);
        QVERIFY2(problems.first().startsWith("settings: "),
                 qPrintable(problems.first()));
    }
};

QTEST_GUILESS_MAIN(PluginsDisabledTest)
#include "plugins_disabled_test.moc"
