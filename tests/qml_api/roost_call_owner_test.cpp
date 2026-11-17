// The owner of a call from QML: a one-shot call (Roost.call, commands.run,
// store.get/set, bus.publish, sessions.refresh) from a panel, whose QQmlContext
// is registered with registerCallOwner, belongs to the panel's owner object, so
// that no callback runs after the panel is gone. A call from the shell, whose
// context is not registered, belongs to the window and is answered as always.
// Against the real roostd and the fixture backend.

#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QSignalSpy>
#include <QTest>

#include <memory>

#include "api/api.hpp"
#include "api/api_test_support.hpp"
#include "helpers/roostd_process.hpp"
#include "qml_api/roost_singleton.hpp"

using namespace roost;
using namespace roost::api_test;

// The static plugin of module Roost.Api, linked into this executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)

namespace {

// One component serves both as a panel and as the shell: the tag tells whose
// callback ran.
const char* const QML = R"(
import QtQml
import Roost.Api

QtObject {
    id: root
    property string tag

    // A callback of the shell: it outlives a panel that hands it over, so
    // that only the owner of the call can stop it.
    function callbackFor(t) {
        return function (e, r) { probe.Hit(t, JSON.stringify(r)) }
    }
    function act(kind) { actWith(kind, callbackFor(tag)) }
    function actWith(kind, cb) {
        if (kind === "call") {
            Roost.call("commands.run", {command: "fixture.echo"}, cb)
        } else if (kind === "run") {
            Roost.commands.run("fixture.echo", undefined, cb)
        } else if (kind === "get") {
            Roost.store.get("fixture", cb)
        } else if (kind === "set") {
            Roost.store.set("fixture", {pins: 5}, cb)
        } else if (kind === "publish") {
            Roost.bus.publish("fixture.event", {a: 1}, cb)
        } else if (kind === "refresh") {
            Roost.sessions.refresh(cb)
        }
    }
    function hang(cb) { Roost.commands.run("fixture.hang", undefined, cb) }
    // A call from an object created inside the panel.
    function nested(cb) {
        var child = Qt.createQmlObject(
            "import QtQml; import Roost.Api; QtObject {" +
            " function go(cb) { Roost.commands.run('fixture.echo', undefined, cb) } }",
            root)
        child.go(cb)
    }
    function watch() {
        var t = tag
        Roost.store.changed.connect(function (plugin, document) {
            probe.Hit(t + ":changed", JSON.stringify(document))
        })
    }
}
)";

// What the callbacks call. It lives in the root context, so it outlives the
// panels.
class Probe : public QObject {
    Q_OBJECT

public:
    Q_INVOKABLE void Hit(const QString& tag, const QString& payload) {
        ++Counts[tag];
        Last[tag] = payload;
    }
    int Count(const QString& tag) const { return Counts.value(tag); }

    QHash<QString, int> Counts;
    QHash<QString, QString> Last;
};

}  // namespace

class RoostCallOwnerTest : public QObject {
    Q_OBJECT

private:
    bool Setup() {
        if (!Daemon_->IsValid() ||
            !Daemon_->AddFixturePlugin("fixture", FIXTURE_BACKEND_PATH) ||
            !Daemon_->Start(ROOSTD_PATH)) {
            return false;
        }
        Api_ = std::make_unique<api::Api>();
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        Api_->Connect(Daemon_->SocketPath());
        if (!connected.wait(TIMEOUT_MS)) {
            return false;
        }
        Engine_ = std::make_unique<QQmlEngine>();
        Engine_->rootContext()->setContextProperty("probe", &Probe_);
        Singleton_ =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        if (Singleton_ == nullptr) {
            return false;
        }
        Singleton_->setApi(*Api_);
        Component_ = std::make_unique<QQmlComponent>(Engine_.get());
        Component_->setData(QML, QUrl());
        Shell_ = Create(Engine_->rootContext(), "shell");
        return Shell_ != nullptr;
    }

    QObject* Create(QQmlContext* context, const char* tag) {
        QObject* object = Component_->create(context);
        if (object == nullptr) {
            qWarning("%s", qPrintable(Component_->errorString()));
            return nullptr;
        }
        QJSEngine::setObjectOwnership(object, QJSEngine::CppOwnership);
        object->setProperty("tag", tag);
        return object;
    }

    // A panel as the loader will make it: its own context, registered with an
    // owner, and the QML in it.
    bool MakePanel() {
        Context_ = new QQmlContext(Engine_->rootContext(), Engine_.get());
        Owner_ = new QObject;
        Singleton_->registerCallOwner(Context_, Owner_);
        Panel_ = Create(Context_, "panel");
        return Panel_ != nullptr;
    }

    // What unloading does, in that order.
    void DestroyOwner() { delete Owner_.data(); }
    void DestroyPanelAndContext() {
        delete Panel_.data();
        delete Context_.data();
    }

    void Act(QObject* object, const char* function, const QString& kind) {
        QVERIFY(
            QMetaObject::invokeMethod(object, function, Q_ARG(QVariant, kind)));
    }
    void Act(QObject* object, const char* function) {
        QVERIFY(QMetaObject::invokeMethod(object, function));
    }
    // A callback made by `maker` (the shell or the panel), for the tag.
    QVariant CallbackOf(QObject* maker, const char* tag) {
        QVariant callback;
        if (!QMetaObject::invokeMethod(maker, "callbackFor",
                                       Q_RETURN_ARG(QVariant, callback),
                                       Q_ARG(QVariant, QString(tag)))) {
            qWarning("callbackFor is not callable");
        }
        return callback;
    }
    // The function of the panel with a callback in the argument.
    void ActWith(QObject* object, const char* function, const QVariant& cb) {
        QVERIFY(
            QMetaObject::invokeMethod(object, function, Q_ARG(QVariant, cb)));
    }
    void ActKind(QObject* object, const QString& kind, const QVariant& cb) {
        QVERIFY(QMetaObject::invokeMethod(
            object, "actWith", Q_ARG(QVariant, kind), Q_ARG(QVariant, cb)));
    }

    // Some turns of the event loop, after what is awaited.
    static void Settle() { QTest::qWait(300); }

    std::unique_ptr<roost::testing::RoostdProcess> Daemon_;
    std::unique_ptr<api::Api> Api_;
    Probe Probe_;
    std::unique_ptr<QQmlEngine> Engine_;
    std::unique_ptr<QQmlComponent> Component_;
    RoostSingleton* Singleton_ = nullptr;
    QPointer<QObject> Shell_;
    QPointer<QQmlContext> Context_;
    QPointer<QObject> Owner_;
    QPointer<QObject> Panel_;

private slots:
    void init() {
        Probe_.Counts.clear();
        Probe_.Last.clear();
        Daemon_ = std::make_unique<roost::testing::RoostdProcess>();
    }

    void cleanup() {
        delete Owner_.data();
        delete Panel_.data();
        delete Context_.data();
        delete Shell_.data();
        Component_.reset();
        // The engine goes first, as in main(): the Api outlives it.
        Engine_.reset();
        Singleton_ = nullptr;
        Api_.reset();
        Daemon_.reset();
    }

    void ACallOfAnUnloadedPanelNeverAnswers_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("callbackOfShell");
        for (const char* kind :
             {"call", "run", "get", "set", "publish", "refresh"}) {
            // A callback of the panel dies with the panel by itself; one of
            // the shell outlives it, and only the owner of the call stops it.
            QTest::newRow(kind) << kind << false;
            QTest::newRow(qPrintable(QString(kind) + "-shell-callback"))
                << kind << true;
        }
    }

    void ACallOfAnUnloadedPanelNeverAnswers() {
        QFETCH(QString, kind);
        QFETCH(bool, callbackOfShell);
        QVERIFY(Setup());
        QVERIFY(MakePanel());
        if (callbackOfShell) {
            ActKind(Panel_, kind, CallbackOf(Shell_, "panel"));
        } else {
            Act(Panel_, "act", kind);
        }
        Act(Shell_, "act", kind);
        DestroyOwner();
        DestroyPanelAndContext();
        // The control: the call of the shell is answered.
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("shell"), 1, TIMEOUT_MS);
        Settle();
        QCOMPARE(Probe_.Count("panel"), 0);
        QCOMPARE(Probe_.Count("shell"), 1);
    }

    void ACallOfAPanelThatIsLiveIsAnswered() {
        QVERIFY(Setup());
        QVERIFY(MakePanel());
        Act(Panel_, "act", "run");
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("panel"), 1, TIMEOUT_MS);
    }

    void AShellCallWithNoRegisteredContextIsAnswered() {
        QVERIFY(Setup());
        Act(Shell_, "act", "run");
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("shell"), 1, TIMEOUT_MS);
    }

    void APendingCallOfAnUnloadedPanelDoesNotAnswerWhenRoostdStops() {
        QVERIFY(Setup());
        QVERIFY(MakePanel());
        QSignalSpy disconnected(Api_.get(), &api::Api::Disconnected);
        ActWith(Panel_, "hang", CallbackOf(Shell_, "panel"));
        // The request is on its way to a backend that never answers.
        Settle();
        DestroyOwner();
        DestroyPanelAndContext();
        // The answer, ConnectionLost, arrives with no one to take it.
        QVERIFY(Daemon_->Stop());
        QVERIFY(disconnected.wait(TIMEOUT_MS));
        Settle();
        QCOMPARE(Probe_.Count("panel"), 0);
    }

    void ACallFromAnObjectInsideAPanelBelongsToThePanel() {
        QVERIFY(Setup());
        QVERIFY(MakePanel());
        ActWith(Panel_, "nested", CallbackOf(Shell_, "panel"));
        Act(Shell_, "act", "run");
        DestroyOwner();
        DestroyPanelAndContext();
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("shell"), 1, TIMEOUT_MS);
        Settle();
        QCOMPARE(Probe_.Count("panel"), 0);
    }

    void ALiveNestedCallIsAnswered() {
        QVERIFY(Setup());
        QVERIFY(MakePanel());
        ActWith(Panel_, "nested", CallbackOf(Panel_, "panel"));
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("panel"), 1, TIMEOUT_MS);
    }

    void ACallAfterTheOwnerIsGoneIsDroppedAndNotSent() {
        QVERIFY(Setup());
        QVERIFY(MakePanel());
        DestroyOwner();
        // The context is still there: the panel is being torn down.
        ActKind(Panel_, "set", CallbackOf(Shell_, "panel"));
        ActKind(Panel_, "run", CallbackOf(Shell_, "panel"));
        Act(Shell_, "act", "get");
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("shell"), 1, TIMEOUT_MS);
        Settle();
        QCOMPARE(Probe_.Count("panel"), 0);
        // roostd never got the write: the document is still empty.
        const QJsonObject got =
            QJsonDocument::fromJson(Probe_.Last.value("shell").toUtf8())
                .object();
        QCOMPARE(got.value("document"), QJsonValue(QJsonObject{}));
    }

    // A probe, not a requirement: whether a handler that a panel connected to
    // Roost.store.changed still runs after the panel is unloaded.
    void AStoreChangedHandlerOfAnUnloadedPanel() {
        QVERIFY(Setup());
        QVERIFY(MakePanel());
        // `changed` follows only the documents that have been read.
        Act(Shell_, "act", "get");
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("shell"), 1, TIMEOUT_MS);
        Act(Panel_, "watch");
        Act(Shell_, "watch");
        DestroyOwner();
        DestroyPanelAndContext();
        Act(Shell_, "act", "set");
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("shell:changed"), 1, TIMEOUT_MS);
        Settle();
        qInfo("store.changed handler of the unloaded panel ran %d time(s)",
              Probe_.Count("panel:changed"));
        QCOMPARE(Probe_.Count("panel:changed"), 0);
    }
};

QTEST_MAIN(RoostCallOwnerTest)
#include "roost_call_owner_test.moc"
