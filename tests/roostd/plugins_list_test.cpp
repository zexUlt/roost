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

class PluginsListTest : public QObject {
    Q_OBJECT

private:
    QString PluginsPath() const { return Dir_->filePath("plugins"); }

    // A plugin without a backend, with the given contributions.
    void AddPlugin(const QString& id, const QString& version,
                   const QJsonObject& contributes) {
        WriteManifest(id, QJsonDocument(QJsonObject{
                                            {"id", id},
                                            {"version", version},
                                            {"api", PLUGIN_API},
                                            {"contributes", contributes},
                                        })
                              .toJson());
    }

    void WriteManifest(const QString& directoryName, const QByteArray& json) {
        const QDir directory(PluginsPath() + u'/' + directoryName);
        QVERIFY(directory.mkpath("."));
        QFile file(directory.filePath("plugin.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(json);
    }

    bool Start() {
        Server_ = std::make_unique<Server>();
        if (!Server_->Listen(Dir_->filePath("roostd.sock"))) {
            return false;
        }
        Host_ = std::make_unique<PluginHost>(*Server_);
        Host_->Load({PluginsPath()});
        Client_ = std::make_unique<RpcClient>();
        const QFuture<rpc::Result> hello =
            Client_->Connect(Dir_->filePath("roostd.sock"));
        return WaitFor(hello) && hello.result();
    }

    rpc::Result List() {
        const QFuture<rpc::Result> future =
            Client_->Request("plugins.list", QJsonValue::Undefined);
        if (!WaitFor(future)) {
            return std::unexpected(rpc::Error{.Message = "timed out"});
        }
        return future.result();
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

    void NoPluginsIsEmpty() {
        QVERIFY(Start());
        const rpc::Result result = List();
        QVERIFY(result);
        QCOMPARE(*result, QJsonValue(QJsonObject{{"plugins", QJsonArray()},
                                                 {"problems", QJsonArray()}}));
    }

    void ListsPluginsInLoadOrder() {
        const QJsonObject first{
            {"commands",
             QJsonArray{QJsonObject{{"id", "b.run"}, {"title", "Run"}}}}};
        const QJsonObject second{{"panels", QJsonArray{"x"}}};
        // Loaded by directory name, not by id.
        AddPlugin("a-dir", "2", QJsonObject{});
        AddPlugin("b-dir", "7", first);
        AddPlugin("c-dir", "1.5", second);
        QVERIFY(Start());
        const rpc::Result result = List();
        QVERIFY2(result, qPrintable(result ? "" : result.error().Message));
        const QJsonObject answer = result->toObject();
        QCOMPARE(answer.value("problems"), QJsonValue(QJsonArray()));
        const QJsonArray plugins = answer.value("plugins").toArray();
        QCOMPARE(plugins.size(), 3);
        const std::vector<PluginManifest>& loaded = Host_->Registry().Plugins();
        QCOMPARE(loaded.size(), size_t{3});
        const QStringList versions{"2", "7", "1.5"};
        const QList<QJsonObject> contributes{QJsonObject{}, first, second};
        for (qsizetype i = 0; i < 3; ++i) {
            const QJsonObject plugin = plugins.at(i).toObject();
            QCOMPARE(plugin.value("id").toString(),
                     loaded[static_cast<size_t>(i)].Id);
            QCOMPARE(plugin.value("version").toString(), versions[i]);
            QCOMPARE(plugin.value("directory").toString(),
                     loaded[static_cast<size_t>(i)].Directory);
            QVERIFY(QDir(plugin.value("directory").toString()).exists());
            QCOMPARE(plugin.value("contributes").toObject(), contributes[i]);
        }
        QCOMPARE(plugins.at(0).toObject().value("id").toString(),
                 QString("a-dir"));
        QCOMPARE(plugins.at(1).toObject().value("id").toString(),
                 QString("b-dir"));
        QCOMPARE(plugins.at(2).toObject().value("id").toString(),
                 QString("c-dir"));
    }

    void PluginsListCarriesNativeOnlyWhenDeclared() {
        AddPlugin("a-plain", "1", QJsonObject{});
        WriteManifest(
            "b-native",
            QJsonDocument(QJsonObject{
                              {"id", "b-native"},
                              {"version", "1"},
                              {"api", PLUGIN_API},
                              {"native", QJsonObject{{"qt", "6.0"},
                                                     {"qmlImportPath", "qml"}}},
                          })
                .toJson());
        // Declared and empty is not the same as not declared.
        WriteManifest("c-empty", QJsonDocument(QJsonObject{
                                                   {"id", "c-empty"},
                                                   {"version", "1"},
                                                   {"api", PLUGIN_API},
                                                   {"native", QJsonObject{}},
                                               })
                                     .toJson());
        // Refused: not in the list, and the reason is a problem.
        WriteManifest(
            "d-library",
            QJsonDocument(QJsonObject{
                              {"id", "d-library"},
                              {"version", "1"},
                              {"api", PLUGIN_API},
                              {"native", QJsonObject{{"library", "lib/x"}}},
                          })
                .toJson());
        QVERIFY(Start());
        const rpc::Result result = List();
        QVERIFY2(result, qPrintable(result ? "" : result.error().Message));
        const QJsonObject answer = result->toObject();
        const QJsonArray plugins = answer.value("plugins").toArray();
        QCOMPARE(plugins.size(), 3);
        const QJsonObject plain = plugins.at(0).toObject();
        QCOMPARE(plain.value("id").toString(), QString("a-plain"));
        QVERIFY(!plain.contains("native"));
        const QJsonObject declared = plugins.at(1).toObject();
        QCOMPARE(declared.value("native").toObject(),
                 (QJsonObject{{"qt", "6.0"}, {"qmlImportPath", "qml"}}));
        const QJsonObject empty = plugins.at(2).toObject();
        QVERIFY(empty.contains("native"));
        QCOMPARE(empty.value("native"), QJsonValue(QJsonObject{}));
        const QJsonArray problems = answer.value("problems").toArray();
        QCOMPARE(problems.size(), 1);
        QVERIFY(problems.at(0).toString().contains("native.library"));
    }

    void BrokenManifestIsAProblem() {
        AddPlugin("good", "1", QJsonObject{});
        WriteManifest("broken", "{ not json");
        QVERIFY(Start());
        const rpc::Result result = List();
        QVERIFY(result);
        const QJsonObject answer = result->toObject();
        const QJsonArray plugins = answer.value("plugins").toArray();
        QCOMPARE(plugins.size(), 1);
        QCOMPARE(plugins.at(0).toObject().value("id").toString(),
                 QString("good"));
        const QJsonArray problems = answer.value("problems").toArray();
        QCOMPARE(problems.size(), 1);
        QCOMPARE(problems.at(0).toString(), Host_->Registry().Problems().at(0));
        QVERIFY(problems.at(0).toString().contains("broken"));
    }
};

QTEST_GUILESS_MAIN(PluginsListTest)
#include "plugins_list_test.moc"
