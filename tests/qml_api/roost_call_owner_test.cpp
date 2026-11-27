// The owner of a call from QML: a one-shot call (Roost.call, commands.run,
// store.get/set, bus.publish, sessions.refresh) from a panel, whose QQmlContext
// is registered with registerCallOwner, belongs to the panel's owner object, so
// that no callback runs after the panel is gone. A call from the shell, whose
// context is not registered, belongs to the window and is answered as always.
// The owner is a child of the context.
// Against the real roostd and the fixture backend.

#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <utility>

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
    // The keys of a command by Roost.call, as a plugin's panel or the shell
    // sends them; what the answer was, error or not, goes to the probe.
    function keys(method, command, t) {
        const params = {command: command}
        if (method === "keymap.setCommand") {
            params.entries = [{key: "Ctrl+Alt+Z"}]
        }
        Roost.call(method, params, function (e, r) {
            probe.Hit(t, JSON.stringify({code: e ? e.code : null,
                                         message: e ? e.message : null}))
        })
    }
    // A call that goes through a function of another object, as a plugin's
    // page that calls a helper of the window: the keys are set there, with the
    // panel's frame still below on the stack.
    property var relay: null
    function through(method, command, t) { relay.keys(method, command, t) }
    // The plugin that the window's own code sees as its caller.
    function caller() { return Roost.callerPlugin() }
    function setPins(n) { Roost.store.set("fixture", {pins: n}, callbackFor(tag)) }
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

// A panel that subscribes to Roost.store.changed in one of three ways (the
// plain "connect" is kept only so that the QML is complete). The
// handlers report to roostProbe, a global of the engine, which does not depend
// on the context of the panel: after unloading, a handler that still runs
// cannot fail on a name that is gone and be taken for one that did not run.
const char* const SIGNAL_QML = R"(
import QtQml
import Roost.Api

QtObject {
    id: root
    property string form

    property Connections connections: Connections {
        target: Roost.store
        enabled: root.form === "connections"
        function onChanged(plugin, document) {
            roostProbe.Hit("panel:connections", JSON.stringify(document))
        }
    }

    Component.onCompleted: {
        if (form === "connect") {
            Roost.store.changed.connect(function (plugin, document) {
                roostProbe.Hit("panel:connect", JSON.stringify(document))
            })
        } else if (form === "connect-target") {
            Roost.store.changed.connect(root, function (plugin, document) {
                roostProbe.Hit("panel:connect-target", JSON.stringify(document))
            })
        }
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
        // The signal may be said inside Connect: look at the spy before
        // waiting.
        if (connected.isEmpty() && !connected.wait(TIMEOUT_MS)) {
            return false;
        }
        Engine_ = std::make_unique<QQmlEngine>();
        Engine_->rootContext()->setContextProperty("probe", &Probe_);
        QJSEngine::setObjectOwnership(&Probe_, QJSEngine::CppOwnership);
        Engine_->globalObject().setProperty("roostProbe",
                                            Engine_->newQObject(&Probe_));
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
    bool MakePanel(const QString& plugin = {}) {
        Context_ = new QQmlContext(Engine_->rootContext(), Engine_.get());
        Owner_ = Singleton_->registerCallOwner(Context_, plugin);
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

    // A panel of a plugin sets the keys of the commands that plugin declares
    // in its `contributes.commands`; the shell, which is no plugin's, sets
    // any. A plugin that asks for another's command is refused with the usual
    // error and the file is not made.
    void APluginSetsTheKeysOfItsOwnCommandsOnly() {
        QVERIFY(Setup());
        const QString file = Daemon_->ConfigPath() + "/keymap.json";
        const auto answer = [&](const char* tag) {
            const QJsonObject object =
                QJsonDocument::fromJson(Probe_.Last.value(tag).toUtf8())
                    .object();
            return std::pair{object.value("code"), object.value("message")};
        };
        const auto keys = [&](QObject* caller, const char* method,
                              const char* command, const char* tag) {
            QVERIFY(QMetaObject::invokeMethod(caller, "keys",
                                              Q_ARG(QVariant, QString(method)),
                                              Q_ARG(QVariant, QString(command)),
                                              Q_ARG(QVariant, QString(tag))));
        };

        // The panel of plugin "other" asks for a command of "fixture".
        QVERIFY(MakePanel("other"));
        keys(Panel_, "keymap.setCommand", "fixture.echo", "set-other");
        keys(Panel_, "keymap.resetCommand", "fixture.echo", "reset-other");
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("set-other"), 1, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("reset-other"), 1, TIMEOUT_MS);
        for (const char* tag : {"set-other", "reset-other"}) {
            const auto [code, message] = answer(tag);
            QCOMPARE(code, QJsonValue(-32602));
            QVERIFY2(message.toString().contains("fixture.echo") &&
                         message.toString().contains("other"),
                     tag);
        }
        // The control: a call after the refused ones was answered by roostd
        // (the shell's), so the refused ones had their turn; the file is
        // still not there.
        keys(Shell_, "keymap.resetCommand", "fixture.echo", "reset-shell");
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("reset-shell"), 1, TIMEOUT_MS);
        QCOMPARE(answer("reset-shell").first, QJsonValue::Null);
        QVERIFY(!QFileInfo::exists(file));

        // The panel of "fixture" sets its own command.
        delete Owner_.data();
        DestroyPanelAndContext();
        QVERIFY(MakePanel("fixture"));
        keys(Panel_, "keymap.setCommand", "fixture.echo", "set-own");
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("set-own"), 1, TIMEOUT_MS);
        QCOMPARE(answer("set-own").first, QJsonValue::Null);
        QFile written(file);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(written.readAll()).array(),
                 (QJsonArray{QJsonObject{{"command", "fixture.echo"},
                                         {"key", "Ctrl+Alt+Z"}}}));
        written.close();

        // The shell sets a command of a plugin that is not its own.
        keys(Shell_, "keymap.resetCommand", "fixture.echo", "reset-own-shell");
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("reset-own-shell"), 1,
                                  TIMEOUT_MS);
        QCOMPARE(answer("reset-own-shell").first, QJsonValue::Null);
    }

    // The shell's function that a plugin's panel calls is judged as the
    // panel's: the call it makes while the panel's frame is on the stack is a
    // call of the plugin, and a call from the shell alone is not.
    void APluginThatCallsAShellFunctionIsStillThePlugin() {
        QVERIFY(Setup());
        const QString file = Daemon_->ConfigPath() + "/keymap.json";
        QVERIFY(MakePanel("other"));
        QVERIFY(
            Panel_->setProperty("relay", QVariant::fromValue(Shell_.data())));

        QVariant seen;
        QVERIFY(QMetaObject::invokeMethod(Shell_, "caller",
                                          Q_RETURN_ARG(QVariant, seen)));
        QCOMPARE(seen.toString(), QString());
        QVERIFY(QMetaObject::invokeMethod(Panel_, "caller",
                                          Q_RETURN_ARG(QVariant, seen)));
        QCOMPARE(seen.toString(), QString("other"));

        QVERIFY(QMetaObject::invokeMethod(
            Panel_, "through", Q_ARG(QVariant, QString("keymap.setCommand")),
            Q_ARG(QVariant, QString("fixture.echo")),
            Q_ARG(QVariant, QString("through-other"))));
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("through-other"), 1, TIMEOUT_MS);
        const QJsonObject refused =
            QJsonDocument::fromJson(Probe_.Last.value("through-other").toUtf8())
                .object();
        QCOMPARE(refused.value("code"), QJsonValue(-32602));
        QVERIFY(!QFileInfo::exists(file));

        // The control: the same helper of the shell, called by the shell.
        QVERIFY(QMetaObject::invokeMethod(
            Shell_, "keys", Q_ARG(QVariant, QString("keymap.resetCommand")),
            Q_ARG(QVariant, QString("fixture.echo")),
            Q_ARG(QVariant, QString("through-shell"))));
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("through-shell"), 1, TIMEOUT_MS);
        QVERIFY(
            QJsonDocument::fromJson(Probe_.Last.value("through-shell").toUtf8())
                .object()
                .value("code")
                .isNull());
    }

    // A singleton of a plugin's own QML module is made in the root context of
    // the engine, below no panel: it is the plugin's by the file it is written
    // in, once the loader says where the plugin is.
    void ASingletonOfThePluginsModuleIsThePlugin() {
        QVERIFY(Setup());
        QTemporaryDir modules;
        QVERIFY(modules.isValid());
        const QString module = modules.filePath("PluginSing");
        QVERIFY(QDir().mkpath(module));
        const auto write = [](const QString& path, const QByteArray& text) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(text) > 0;
        };
        QVERIFY(write(module + "/qmldir",
                      "module PluginSing\nsingleton Sing 1.0 Sing.qml\n"));
        QVERIFY(write(module + "/Sing.qml", R"(
pragma Singleton
import QtQml
import Roost.Api
QtObject {
    function keys(method, command, t) {
        Roost.call(method, {command: command}, function (e, r) {
            probe.Hit(t, JSON.stringify({code: e ? e.code : null}))
        })
    }
}
)"));
        Engine_->addImportPath(modules.path());
        QQmlComponent user(Engine_.get());
        user.setData(
            "import QtQml\nimport PluginSing 1.0\nQtObject {"
            " function go(t) { Sing.keys('keymap.resetCommand', "
            "'fixture.echo', t) } }",
            QUrl());
        QObject* caller = user.create(Engine_->rootContext());
        QVERIFY2(caller != nullptr, qPrintable(user.errorString()));
        QJSEngine::setObjectOwnership(caller, QJSEngine::CppOwnership);
        // The code of the answer, "timed out" when none came.
        const auto go = [&](const QString& tag) -> QJsonValue {
            if (!QMetaObject::invokeMethod(caller, "go",
                                           Q_ARG(QVariant, tag)) ||
                !QTest::qWaitFor([&] { return Probe_.Count(tag) == 1; },
                                 TIMEOUT_MS)) {
                return QStringLiteral("timed out");
            }
            return QJsonDocument::fromJson(Probe_.Last.value(tag).toUtf8())
                .object()
                .value("code");
        };

        // Nobody knows whose file it is: the window's.
        QCOMPARE(go("unknown"), QJsonValue::Null);
        // The loader says the directory is plugin "other"'s.
        Singleton_->addPluginDirectory("other", modules.path());
        QCOMPARE(go("known"), QJsonValue(-32602));
        // The plugin goes: the file is no one's again.
        Singleton_->dropPluginDirectory("other");
        QCOMPARE(go("dropped"), QJsonValue::Null);
        // The plugin that declares the command is let through by its files.
        Singleton_->addPluginDirectory("fixture", modules.path());
        QCOMPARE(go("own"), QJsonValue::Null);
        delete caller;
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

    void ACallOfAPanelWhoseContextIsDeletedNeverAnswers_data() {
        QTest::addColumn<QString>("kind");
        for (const char* kind :
             {"call", "run", "get", "set", "publish", "refresh"}) {
            QTest::newRow(kind) << kind;
        }
    }

    // The owner is never deleted by hand: it goes with the context, in the
    // same call stack, whatever the order of unloading.
    void ACallOfAPanelWhoseContextIsDeletedNeverAnswers() {
        QFETCH(QString, kind);
        QVERIFY(Setup());
        QVERIFY(MakePanel());
        QVERIFY(Owner_ != nullptr);
        ActKind(Panel_, kind, CallbackOf(Shell_, "panel"));
        Act(Shell_, "act", kind);
        DestroyPanelAndContext();
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("shell"), 1, TIMEOUT_MS);
        Settle();
        QCOMPARE(Probe_.Count("panel"), 0);
        QVERIFY(Owner_.isNull());
    }

    void ADeletedContextTakesItsOwnerWithIt() {
        QVERIFY(Setup());
        QVERIFY(MakePanel());
        QCOMPARE(Owner_->parent(), Context_.data());
        delete Context_.data();
        QVERIFY(Owner_.isNull());
    }

    void AStoreChangedHandlerOfAnUnloadedPanel_data() {
        QTest::addColumn<QString>("form");
        QTest::newRow("connections") << "connections";
        // Not here: a plain Roost.store.changed.connect(fn) outlives the panel
        // until an explicit disconnect.
        QTest::newRow("connect-target") << "connect-target";
    }

    void AStoreChangedHandlerOfAnUnloadedPanel() {
        QFETCH(QString, form);
        QVERIFY(Setup());
        Context_ = new QQmlContext(Engine_->rootContext(), Engine_.get());
        Owner_ = Singleton_->registerCallOwner(Context_);
        QQmlComponent component(Engine_.get());
        component.setData(SIGNAL_QML, QUrl());
        Panel_ = component.beginCreate(Context_);
        QVERIFY2(Panel_ != nullptr, qPrintable(component.errorString()));
        QJSEngine::setObjectOwnership(Panel_, QJSEngine::CppOwnership);
        Panel_->setProperty("form", form);
        component.completeCreate();
        // `changed` follows only the documents that have been read.
        Act(Shell_, "act", "get");
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("shell"), 1, TIMEOUT_MS);
        Shell_->setProperty("tag", "shell-set");
        QVERIFY(
            QMetaObject::invokeMethod(Shell_, "setPins", Q_ARG(QVariant, 1)));
        // The control: before unloading the handler runs.
        const QString tag = "panel:" + form;
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count(tag), 1, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("shell-set"), 1, TIMEOUT_MS);
        DestroyOwner();
        DestroyPanelAndContext();
        QVERIFY(
            QMetaObject::invokeMethod(Shell_, "setPins", Q_ARG(QVariant, 2)));
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("shell-set"), 2, TIMEOUT_MS);
        Settle();
        QCOMPARE(Probe_.Count(tag), 1);
    }
};

QTEST_MAIN(RoostCallOwnerTest)
#include "roost_call_owner_test.moc"
