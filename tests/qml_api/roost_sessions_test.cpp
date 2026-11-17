// Roost.sessions, the list model over the session cache of the window, in a
// QQmlEngine over the real roostd with a plugin whose backend is the fixture
// backend. A QAbstractItemModelTester watches the model
// through every test.

#include <QAbstractItemModelTester>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QSet>
#include <QSignalSpy>
#include <QTest>

#include <memory>

#include "api/api.hpp"
#include "api/api_test_support.hpp"
#include "core/protocol.hpp"
#include "helpers/roostd_process.hpp"
#include "qml_api/roost_singleton.hpp"
#include "qml_api/sessions_model.hpp"

using namespace roost;
using namespace roost::api_test;

// The static plugin of module Roost.Api, linked into this executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)

namespace {

const QString AGENT = QStringLiteral("fixture-agent");

QJsonObject Row(const QString& id, const QString& state,
                const QString& title = {}) {
    return {{"agent", AGENT},
            {"id", id},
            {"title", title},
            {"cwd", ""},
            {"state", state}};
}

QJsonObject WorkingRow() {
    return {{"agent", AGENT},     {"id", "s2"},
            {"title", "second"},  {"cwd", "/tmp"},
            {"state", "working"}, {"startedAt", qint64{1700000000000}}};
}

// agent.json of the fixture plugin: what its backend answers to
// agent.listSessions, read anew for each request.
QJsonObject Answers(const QJsonArray& sessions) {
    return {{"agent.listSessions",
             QJsonObject{{AGENT, QJsonObject{{"result", sessions}}}}}};
}

QJsonObject Removed(const QString& id) {
    return {{"jsonrpc", "2.0"},
            {"method", "session.removed"},
            {"params", QJsonObject{{"agent", AGENT}, {"session", id}}}};
}

QJsonObject StateChanged(const QJsonObject& session) {
    return {{"jsonrpc", "2.0"},
            {"method", "session.stateChanged"},
            {"params", QJsonObject{{"agent", AGENT}, {"session", session}}}};
}

const char* const QML = R"(
import QtQuick
import Roost.Api

Item {
    id: root
    property var got: []

    ListView {
        id: list
        width: 200
        height: 600
        model: Roost.sessions
        delegate: Item {
            width: 200
            height: 20
            property string key: model.agent + "/" + model.id
            property string st: model.state
        }
    }

    function count() { return list.count }
    // The view has no window, so nothing polishes it: lay it out by hand.
    function keys() {
        list.forceLayout()
        var r = []
        for (var i = 0; i < list.count; i++) {
            var item = list.itemAtIndex(i)
            r.push(item ? item.key : null)
        }
        return r
    }
    function states() {
        list.forceLayout()
        var r = []
        for (var i = 0; i < list.count; i++) {
            var item = list.itemAtIndex(i)
            r.push(item ? item.st : null)
        }
        return r
    }
    // Whether the callback ran inside the call itself.
    function refreshRunsInside() {
        var hit = false
        Roost.sessions.refresh(function (e, r) {
            hit = true
            got.push(JSON.stringify({err: e, n: r ? r.sessions.length : -1}))
            gotChanged()
        })
        return hit
    }
}
)";

}  // namespace

class RoostSessionsTest : public QObject {
    Q_OBJECT

private:
    QString AgentFile() const {
        return Daemon_->PluginsPath() + "/fixture/agent.json";
    }

    bool SetAnswers(const QJsonObject& answers) {
        QFile file(AgentFile());
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               file.write(QJsonDocument(answers).toJson()) > 0;
    }

    // The daemon with the sessions s1 (idle) and s2 (working), unless the
    // answers are given; then the Api that is not connected yet, the engine
    // and the QML object, and the model tester on Roost.sessions.
    bool Prepare(const QJsonObject& answers = {}) {
        if (!Daemon_->IsValid() ||
            !Daemon_->AddFixturePlugin("fixture", FIXTURE_BACKEND_PATH) ||
            !SetAnswers(answers.isEmpty()
                            ? Answers({Row("s1", "idle"), WorkingRow()})
                            : answers) ||
            !Daemon_->Start(ROOSTD_PATH)) {
            return false;
        }
        // Short retry delays: a restart of roostd under load must not push
        // the next attempt past the test's timeout.
        Api_ = std::make_unique<api::Api>(api::ApiOptions{
            .FirstReconnectDelay = std::chrono::milliseconds{20},
            .MaxReconnectDelay = std::chrono::milliseconds{80}});
        Engine_ = std::make_unique<QQmlEngine>();
        Singleton_ =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        if (Singleton_ == nullptr) {
            return false;
        }
        Singleton_->setApi(*Api_);
        Model_ = Singleton_->sessions();
        if (Model_ == nullptr) {
            return false;
        }
        Tester_ = std::make_unique<QAbstractItemModelTester>(
            Model_, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QQmlComponent component(Engine_.get());
        component.setData(QML, QUrl());
        Page_.reset(component.create());
        if (!Page_) {
            qWarning("%s", qPrintable(component.errorString()));
            return false;
        }
        return true;
    }

    // Connects and waits for the first list.
    bool ConnectApi() {
        Api_->Connect(Daemon_->SocketPath());
        return QTest::qWaitFor([this] { return Model_->loaded(); }, TIMEOUT_MS);
    }

    // Has the backend send these notifications to roostd.
    bool Notify(const QJsonArray& messages) {
        const QFuture<rpc::Result> sent =
            Api_->Commands().Run("fixture.notify", messages);
        return WaitFor(sent) && sent.result();
    }

    QVariant Role(int row, const QString& name) const {
        return Model_->data(Model_->index(row), RoleOf(name));
    }

    int RoleOf(const QString& name) const {
        return Model_->roleNames().key(name.toUtf8(), -1);
    }

    QVariant Call(const char* function) {
        QVariant result;
        QMetaObject::invokeMethod(Page_.get(), function,
                                  Q_RETURN_ARG(QVariant, result));
        return result;
    }

    std::unique_ptr<roost::testing::RoostdProcess> Daemon_;
    std::unique_ptr<api::Api> Api_;
    std::unique_ptr<QQmlEngine> Engine_;
    RoostSingleton* Singleton_ = nullptr;
    SessionsModel* Model_ = nullptr;
    std::unique_ptr<QAbstractItemModelTester> Tester_;
    std::unique_ptr<QObject> Page_;

private slots:
    void init() { Daemon_ = std::make_unique<roost::testing::RoostdProcess>(); }

    void cleanup() {
        // The tester and the engine go first, as in main(): the Api outlives
        // them.
        Tester_.reset();
        Page_.reset();
        Engine_.reset();
        Singleton_ = nullptr;
        Model_ = nullptr;
        Api_.reset();
        Daemon_.reset();
    }

    void TheRolesAreTheKeysOfARealRow() {
        QVERIFY(Prepare());
        QVERIFY(!Model_->loaded());
        QCOMPARE(Model_->rowCount(), 0);
        QVERIFY(ConnectApi());
        // s2 is the row that has startedAt, the key that is optional.
        const QJsonObject row = Api_->Sessions().Rows()[1];
        QSet<QString> keys;
        for (const QString& key : row.keys()) {
            keys.insert(key);
        }
        QSet<QString> roles;
        for (const QByteArray& name : Model_->roleNames()) {
            roles.insert(QString::fromUtf8(name));
        }
        QCOMPARE(roles, keys);
        QCOMPARE(roles.size(), api::SessionService::FIELDS.size());
        // In the order of FIELDS, from Qt::UserRole.
        for (int i = 0; i < api::SessionService::FIELDS.size(); ++i) {
            QCOMPARE(Model_->roleNames().value(Qt::UserRole + i),
                     api::SessionService::FIELDS[i].toUtf8());
        }
    }

    void TheValuesAreTheRowsOfTheService() {
        QVERIFY(Prepare());
        QVERIFY(ConnectApi());
        const auto& rows = Api_->Sessions().Rows();
        QCOMPARE(Model_->rowCount(), 2);
        QCOMPARE(rows.size(), 2);
        for (int i = 0; i < rows.size(); ++i) {
            for (const QString& key : api::SessionService::FIELDS) {
                QCOMPARE(Role(i, key), rows[i].value(key).toVariant());
            }
        }
        QCOMPARE(Role(1, "id"), QVariant("s2"));
        QCOMPARE(Role(1, "startedAt").toLongLong(), qint64{1700000000000});
        // A key the row lacks is an invalid variant, undefined in QML.
        QVERIFY(!rows[0].contains("startedAt"));
        QVERIFY(!Role(0, "startedAt").isValid());
        QVERIFY(Model_->unavailable().isEmpty());
        QVERIFY(Model_->loaded());
    }

    void AStateChangeIsADataChangeAndANewSessionAnInsert() {
        QVERIFY(Prepare());
        QVERIFY(ConnectApi());
        QSignalSpy changed(Model_, &QAbstractItemModel::dataChanged);
        QSignalSpy inserted(Model_, &QAbstractItemModel::rowsInserted);
        QVERIFY(Notify({StateChanged(Row("s2", "done", "finished"))}));
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, TIMEOUT_MS);
        QCOMPARE(changed[0][0].toModelIndex().row(), 1);
        QCOMPARE(changed[0][1].toModelIndex().row(), 1);
        QCOMPARE(Role(1, "state"), QVariant("done"));
        QCOMPARE(Role(1, "title"), QVariant("finished"));
        QVERIFY(!Role(1, "startedAt").isValid());
        QVERIFY(inserted.isEmpty());
        QCOMPARE(Model_->rowCount(), 2);

        QVERIFY(Notify({StateChanged(Row("s3", "blocked", "new"))}));
        QTRY_COMPARE_WITH_TIMEOUT(inserted.size(), 1, TIMEOUT_MS);
        QCOMPARE(inserted[0][1].toInt(), 2);
        QCOMPARE(inserted[0][2].toInt(), 2);
        QCOMPARE(Model_->rowCount(), 3);
        QCOMPARE(Role(2, "id"), QVariant("s3"));
        QCOMPARE(changed.size(), 1);
    }

    void ARemovalIsARowRemoval() {
        QVERIFY(Prepare());
        QVERIFY(ConnectApi());
        QStringList told;
        QObject scope;
        // The model still has the row, and the count that the views were told,
        // when it is about to go; the tester checks the rest.
        connect(Model_, &QAbstractItemModel::rowsAboutToBeRemoved, &scope,
                [&](const QModelIndex&, int first, int last) {
                    told.append(QStringLiteral("about %1-%2 %3 of %4")
                                    .arg(first)
                                    .arg(last)
                                    .arg(Role(first, "id").toString())
                                    .arg(Model_->rowCount()));
                });
        connect(Model_, &QAbstractItemModel::rowsRemoved, &scope,
                [&](const QModelIndex&, int first, int last) {
                    told.append(QStringLiteral("removed %1-%2 of %3")
                                    .arg(first)
                                    .arg(last)
                                    .arg(Model_->rowCount()));
                });
        QSignalSpy reset(Model_, &QAbstractItemModel::modelReset);
        QVERIFY(Notify({Removed("s1")}));
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 2, TIMEOUT_MS);
        QCOMPARE(told, (QStringList{"about 0-0 s1 of 2", "removed 0-0 of 1"}));
        QCOMPARE(Model_->rowCount(), 1);
        QCOMPARE(Role(0, "id"), QVariant("s2"));
        QVERIFY(reset.isEmpty());
    }

    void AnAdapterThatCrashedIsUnavailable() {
        QVERIFY(
            Prepare({{"agent.listSessions", QJsonObject{{AGENT, "crash"}}}}));
        QSignalSpy unavailableChanged(Model_,
                                      &SessionsModel::unavailableChanged);
        QSignalSpy loadedChanged(Model_, &SessionsModel::loadedChanged);
        QVERIFY(ConnectApi());
        QCOMPARE(Model_->rowCount(), 0);
        QVERIFY(unavailableChanged.size() >= 1);
        QVERIFY(loadedChanged.size() >= 1);
        const QVariantList unavailable = Model_->unavailable();
        QCOMPARE(unavailable.size(), 1);
        const QVariantMap entry = unavailable[0].toMap();
        QCOMPARE(entry.value("agent").toString(), AGENT);
        QVERIFY(!entry.value("reason").toString().isEmpty());
    }

    void RefreshAnswersItsCallbackLater() {
        QVERIFY(Prepare());
        QVERIFY(ConnectApi());
        QSignalSpy reset(Model_, &QAbstractItemModel::modelReset);
        QSignalSpy loadedChanged(Model_, &SessionsModel::loadedChanged);
        QSignalSpy unavailableChanged(Model_,
                                      &SessionsModel::unavailableChanged);
        QVERIFY(SetAnswers(Answers({Row("s9", "done"), Row("s1", "failed")})));
        QVERIFY(!Call("refreshRunsInside").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(Page_->property("got").toList().size(), 1,
                                  TIMEOUT_MS);
        const QJsonObject record =
            QJsonDocument::fromJson(
                Page_->property("got").toList()[0].toString().toUtf8())
                .object();
        QVERIFY(record.value("err").isNull());
        QCOMPARE(record.value("n").toInt(), 2);
        // The rows were replaced before the callback.
        QCOMPARE(reset.size(), 1);
        QCOMPARE(Model_->rowCount(), 2);
        QCOMPARE(Role(0, "id"), QVariant("s9"));
        QCOMPARE(Role(1, "id"), QVariant("s1"));
        QCOMPARE(Role(1, "state"), QVariant("failed"));
        // Loaded before and after, nobody unavailable: nothing to tell, so a
        // handler that refreshes on loadedChanged would not loop.
        QCOMPARE(loadedChanged.size(), 0);
        QCOMPARE(unavailableChanged.size(), 0);
    }

    void ALostConnectionEmptiesTheModelAndTheReconnectFillsIt() {
        QVERIFY(Prepare());
        QVERIFY(ConnectApi());
        QCOMPARE(Model_->rowCount(), 2);
        QVERIFY(Daemon_->Stop());
        QTRY_COMPARE_WITH_TIMEOUT(Model_->rowCount(), 0, TIMEOUT_MS);
        QVERIFY(!Model_->loaded());
        QVERIFY(Model_->unavailable().isEmpty());
        // Refresh with no connection fails, and the rows stay empty.
        QVERIFY(!Call("refreshRunsInside").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(Page_->property("got").toList().size(), 1,
                                  TIMEOUT_MS);
        QVERIFY(!QJsonDocument::fromJson(
                     Page_->property("got").toList()[0].toString().toUtf8())
                     .object()
                     .value("err")
                     .isNull());
        QCOMPARE(Model_->rowCount(), 0);

        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QTRY_COMPARE_WITH_TIMEOUT(Model_->rowCount(), 2, TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(Model_->loaded(), TIMEOUT_MS);
        QCOMPARE(Role(0, "id"), QVariant("s1"));
        QCOMPARE(Role(1, "id"), QVariant("s2"));
    }

    void AListViewDelegateReadsTheRolesByName() {
        QVERIFY(Prepare());
        QVERIFY(ConnectApi());
        const QStringList both{AGENT + "/s1", AGENT + "/s2"};
        QTRY_COMPARE_WITH_TIMEOUT(Call("keys").toStringList(), both,
                                  TIMEOUT_MS);
        QCOMPARE(Call("count").toInt(), 2);
        QCOMPARE(Call("states").toStringList(),
                 (QStringList{"idle", "working"}));
        // A change reaches the delegate, and so does an insert.
        QVERIFY(Notify({StateChanged(Row("s1", "done")),
                        StateChanged(Row("s3", "idle"))}));
        QTRY_COMPARE_WITH_TIMEOUT(Call("states").toStringList(),
                                  (QStringList{"done", "working", "idle"}),
                                  TIMEOUT_MS);
        QCOMPARE(Call("keys").toStringList(),
                 (QStringList{AGENT + "/s1", AGENT + "/s2", AGENT + "/s3"}));
        // A removal takes the row out of the view.
        QVERIFY(Notify({Removed("s2")}));
        QTRY_COMPARE_WITH_TIMEOUT(Call("states").toStringList(),
                                  (QStringList{"done", "idle"}), TIMEOUT_MS);
        QCOMPARE(Call("keys").toStringList(),
                 (QStringList{AGENT + "/s1", AGENT + "/s3"}));
    }
};

QTEST_MAIN(RoostSessionsTest)
#include "roost_sessions_test.moc"
