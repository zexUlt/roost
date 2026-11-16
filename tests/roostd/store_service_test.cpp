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

#include <memory>

#include "core/frame.hpp"
#include "core/protocol.hpp"
#include "core/rpc_client.hpp"
#include "core/store.hpp"
#include "roostd/plugins.hpp"
#include "roostd/server.hpp"
#include "roostd/store_service.hpp"

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

int CodeOf(const rpc::Result& result) {
    return result ? 0 : result.error().Code;
}

QString MessageOf(const rpc::Result& result) {
    return result ? QString() : result.error().Message;
}

constexpr int INVALID_PARAMS = static_cast<int>(rpc::EErrorCode::InvalidParams);
constexpr int STORE_FAILED =
    static_cast<int>(protocol::EErrorCode::StoreFailed);

}  // namespace

class StoreServiceTest : public QObject {
    Q_OBJECT

private:
    QString PluginsPath() const { return Dir_->filePath("plugins"); }
    QString ConfigPath() const { return Dir_->filePath("config"); }
    QString FileOf(const QString& id) const {
        return ConfigPath() + u'/' + id + ".json";
    }

    // A plugin without a backend.
    void AddPlugin(const QString& id) {
        const QDir directory(PluginsPath() + u'/' + id);
        QVERIFY(directory.mkpath("."));
        QFile file(directory.filePath("plugin.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject{
                                     {"id", id},
                                     {"version", "1"},
                                     {"api", PLUGIN_API},
                                 })
                       .toJson());
    }

    std::unique_ptr<RpcClient> Connect() {
        auto client = std::make_unique<RpcClient>();
        const QFuture<rpc::Result> hello =
            client->Connect(Dir_->filePath("roostd.sock"));
        if (!WaitFor(hello) || !hello.result()) {
            return nullptr;
        }
        return client;
    }

    // The server, the plugins of PluginsPath() and the service over
    // ConfigPath().
    bool Start() {
        Server_ = std::make_unique<Server>();
        if (!Server_->Listen(Dir_->filePath("roostd.sock"))) {
            return false;
        }
        Host_ = std::make_unique<PluginHost>(*Server_);
        Host_->Load({PluginsPath()});
        Service_ = std::make_unique<StoreService>(Store(ConfigPath()), *Server_,
                                                  Host_->Registry());
        Client_ = Connect();
        return Client_ != nullptr;
    }

    rpc::Result Call(const QString& method, const QJsonValue& params) {
        const QFuture<rpc::Result> future = Client_->Request(method, params);
        if (!WaitFor(future)) {
            return std::unexpected(rpc::Error{.Message = "timed out"});
        }
        return future.result();
    }

    std::unique_ptr<QTemporaryDir> Dir_;
    std::unique_ptr<Server> Server_;
    std::unique_ptr<PluginHost> Host_;
    std::unique_ptr<StoreService> Service_;
    std::unique_ptr<RpcClient> Client_;

private slots:
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
    }

    void cleanup() {
        Client_.reset();
        Service_.reset();
        Host_.reset();
        Server_.reset();
    }

    void GetOfAnUnknownPluginIsInvalid() {
        AddPlugin("known");
        QVERIFY(Start());
        const rpc::Result result =
            Call("store.get", QJsonObject{{"plugin", "x"}});
        QCOMPARE(CodeOf(result), INVALID_PARAMS);
        for (const QJsonObject& params :
             {QJsonObject{}, QJsonObject{{"plugin", 1}}}) {
            QCOMPARE(CodeOf(Call("store.get", params)), INVALID_PARAMS);
        }
    }

    void GetBeforeAnySetIsEmpty() {
        AddPlugin("known");
        QVERIFY(Start());
        const rpc::Result result =
            Call("store.get", QJsonObject{{"plugin", "known"}});
        QVERIFY2(result, qPrintable(MessageOf(result)));
        QCOMPARE(*result, QJsonValue(QJsonObject{{"document", QJsonObject{}}}));
    }

    void SetThenGetReturnsTheDocument() {
        AddPlugin("known");
        QVERIFY(Start());
        const QJsonObject document{{"pins", QJsonArray{"a", "b"}}, {"n", 3}};
        const rpc::Result set =
            Call("store.set",
                 QJsonObject{{"plugin", "known"}, {"document", document}});
        QVERIFY2(set, qPrintable(MessageOf(set)));
        QCOMPARE(*set, QJsonValue());
        QVERIFY(QFile::exists(FileOf("known")));

        const rpc::Result got =
            Call("store.get", QJsonObject{{"plugin", "known"}});
        QVERIFY2(got, qPrintable(MessageOf(got)));
        QCOMPARE(*got, QJsonValue(QJsonObject{{"document", document}}));

        // A whole-document write: the second replaces the first.
        const QJsonObject second{{"only", true}};
        QVERIFY(Call("store.set",
                     QJsonObject{{"plugin", "known"}, {"document", second}}));
        QCOMPARE(*Call("store.get", QJsonObject{{"plugin", "known"}}),
                 QJsonValue(QJsonObject{{"document", second}}));
    }

    void SetIsSeenOnAnotherConnection() {
        AddPlugin("known");
        QVERIFY(Start());
        const auto listener = Connect();
        QVERIFY(listener);
        QSignalSpy heard(listener.get(), &RpcClient::NotificationReceived);
        QVERIFY(WaitFor(listener->Request(
            "bus.subscribe", QJsonObject{{"pattern", "store.*"}})));

        const QJsonObject document{{"k", "v"}};
        QVERIFY(Call("store.set",
                     QJsonObject{{"plugin", "known"}, {"document", document}}));
        QTRY_COMPARE_WITH_TIMEOUT(heard.count(), 1, TIMEOUT_MS);
        QCOMPARE(heard.at(0).at(0).toString(), QString("bus.event"));
        const QJsonObject event =
            heard.at(0).at(1).value<QJsonValue>().toObject();
        QCOMPARE(event.value("name"), QJsonValue("store.changed"));
        QCOMPARE(event.value("payload"),
                 QJsonValue(
                     QJsonObject{{"plugin", "known"}, {"document", document}}));
    }

    void NonObjectDocumentIsInvalid() {
        AddPlugin("known");
        QVERIFY(Start());
        for (const QJsonValue& document :
             {QJsonValue(QJsonArray{1}), QJsonValue("text"), QJsonValue(1),
              QJsonValue(), QJsonValue(QJsonValue::Null)}) {
            QJsonObject params{{"plugin", "known"}};
            if (!document.isUndefined()) {
                params.insert("document", document);
            }
            QCOMPARE(CodeOf(Call("store.set", params)), INVALID_PARAMS);
        }
        QCOMPARE(
            CodeOf(Call("store.set", QJsonObject{{"plugin", "nobody"},
                                                 {"document", QJsonObject{}}})),
            INVALID_PARAMS);
        QVERIFY(!QFile::exists(FileOf("known")));
        QVERIFY(!QFile::exists(FileOf("nobody")));
    }

    void DocumentTooBigForAnEventIsInvalid() {
        AddPlugin("known");
        QVERIFY(Start());
        const auto listener = Connect();
        QVERIFY(listener);
        QSignalSpy heard(listener.get(), &RpcClient::NotificationReceived);
        QVERIFY(WaitFor(listener->Request(
            "bus.subscribe", QJsonObject{{"pattern", "store.*"}})));

        // The request fits a frame, the event it makes does not: it is a few
        // bytes longer.
        const auto paramsOf = [](qsizetype size) {
            return QJsonObject{
                {"plugin", "known"},
                {"document", QJsonObject{{"x", QString(size, u'x')}}}};
        };
        const auto requestSize = [&](qsizetype size) {
            return QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                             {"id", 99999},
                                             {"method", "store.set"},
                                             {"params", paramsOf(size)}})
                .toJson(QJsonDocument::Compact)
                .size();
        };
        const qsizetype size = qsizetype{MAX_FRAME_LENGTH} - 1 - requestSize(0);
        QVERIFY(requestSize(size) <= qsizetype{MAX_FRAME_LENGTH} - 1);
        QVERIFY(!Server::EventFitsAFrame(
            "store.changed",
            QJsonObject{{"plugin", "known"},
                        {"document", paramsOf(size).value("document")}}));

        const rpc::Result result = Call("store.set", paramsOf(size));
        QCOMPARE(CodeOf(result), INVALID_PARAMS);
        QVERIFY(!QFile::exists(FileOf("known")));
        QTest::qWait(20);
        QCOMPARE(heard.count(), 0);
        // The connection and the service are fine.
        QVERIFY(Call("store.get", QJsonObject{{"plugin", "known"}}));
    }

    void UnparseableFileFailsGetAndStaysAsItIs() {
        AddPlugin("known");
        QVERIFY(QDir().mkpath(ConfigPath()));
        const QByteArray garbage = "{ not json";
        {
            QFile file(FileOf("known"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(garbage);
        }
        QVERIFY(Start());
        const rpc::Result result =
            Call("store.get", QJsonObject{{"plugin", "known"}});
        QCOMPARE(CodeOf(result), STORE_FAILED);
        QVERIFY2(!MessageOf(result).isEmpty(), "the store's message");
        QFile file(FileOf("known"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), garbage);
    }

    void DocumentTooBigForAFrameIsStoreFailedAndConnectionLives() {
        AddPlugin("known");
        QVERIFY(QDir().mkpath(ConfigPath()));
        {
            // One string value just over the frame limit; valid JSON.
            QFile file(FileOf("known"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{\"blob\":\"");
            file.write(QByteArray(20 * 1024 * 1024, 'x'));
            file.write("\"}");
        }
        QVERIFY(Start());
        const rpc::Result result =
            Call("store.get", QJsonObject{{"plugin", "known"}});
        QCOMPARE(CodeOf(result), STORE_FAILED);
        QVERIFY2(MessageOf(result).contains("too big"),
                 qPrintable(MessageOf(result)));
        // The connection is still open.
        QCOMPARE(CodeOf(Call("store.get", QJsonObject{{"plugin", "known"}})),
                 STORE_FAILED);
    }

    void SaveFailureIsStoreFailedAndNotAnnounced() {
        AddPlugin("known");
        // The config path is a file, so the directory cannot be created.
        {
            QFile file(ConfigPath());
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        QVERIFY(Start());
        const auto listener = Connect();
        QVERIFY(listener);
        QSignalSpy heard(listener.get(), &RpcClient::NotificationReceived);
        QVERIFY(WaitFor(listener->Request(
            "bus.subscribe", QJsonObject{{"pattern", "store.*"}})));
        const rpc::Result result =
            Call("store.set", QJsonObject{{"plugin", "known"},
                                          {"document", QJsonObject{{"a", 1}}}});
        QCOMPARE(CodeOf(result), STORE_FAILED);
        QTest::qWait(20);
        QCOMPARE(heard.count(), 0);
    }
};

QTEST_GUILESS_MAIN(StoreServiceTest)
#include "store_service_test.moc"
