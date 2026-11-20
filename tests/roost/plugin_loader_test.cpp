// PluginLoader, the window's loader of the QML panels of plugins, against the
// real roostd (plugins with QML panels and the fixture backend) and a real
// QQmlEngine with the singleton Roost: what loads, what a broken panel
// disables, how a changed plugin is reloaded and an unchanged one is left
// alone, and that a torn-down panel leaves no call and no subscription behind.

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "api/api.hpp"
#include "api/api_test_support.hpp"
#include "helpers/roostd_process.hpp"
#include "qml_api/roost_singleton.hpp"
#include "roost/plugin_loader.hpp"

using namespace roost;
using namespace roost::api_test;

// The static plugin of module Roost.Api, linked into this executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)

Q_DECLARE_METATYPE(QQuickItem*)

namespace {

// What the panels report to; a global of the engine, so that it does not
// depend on the context of a panel that is gone.
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

// A panel whose root is an Item; `content` tells which version of the file it
// is and which panel, and onCompleted reports the pluginId it sees.
QByteArray ItemPanel(const QString& content) {
    return QStringLiteral(R"(
import QtQuick
Item {
    property string pluginId
    property string content: "%1"
    Component.onCompleted: roostProbe.Hit("completed:" + content, pluginId)
}
)")
        .arg(content)
        .toUtf8();
}

// A panel that has no pluginId.
const QByteArray PLAIN_PANEL = R"(
import QtQuick
Item {
    property string content: "plain"
    Component.onCompleted: roostProbe.Hit("completed:plain", "ran")
}
)";

const QByteArray OBJECT_PANEL = R"(
import QtQml
QtObject {
    property string content: "object"
}
)";

const QByteArray SYNTAX_ERROR_PANEL = "import QtQuick\nItem {{\n";
const QByteArray UNKNOWN_TYPE_PANEL = "import QtQuick\nNoSuchType {}\n";

// A panel with the properties `session` and `selected` only.
const QByteArray PORTS_PANEL = R"(
import QtQuick
Item {
    property var session: null
    property var selected: null
}
)";

// A panel that counts its lives; and one whose pluginId is required.
const QByteArray LIFE_PANEL = R"(
import QtQuick
Item {
    property string content: "life"
    Component.onCompleted: roostProbe.Hit("completed:life", "")
    Component.onDestruction: roostProbe.Hit("destroyed:life", "")
}
)";

const QByteArray REQUIRED_PANEL = R"(
import QtQuick
Item {
    required property string pluginId
    Component.onCompleted: roostProbe.Hit("completed:required", pluginId)
}
)";

// A panel that writes to the store of its plugin while it is destroyed: the
// call is the panel's own, so it must be dropped if the owner of the calls is
// deleted before the item.
const QByteArray FAREWELL_PANEL = R"(
import QtQuick
import Roost.Api
Item {
    function save() { Roost.store.set("a-one", {live: 1}) }
    Component.onDestruction: Roost.store.set("a-one", {farewell: 1})
}
)";

// A panel that calls into Roost and subscribes, with the callbacks given.
const QByteArray CALLS_PANEL = R"(
import QtQuick
import Roost.Api
Item {
    id: root
    property string content: "calls"
    function run(kind, cb) {
        if (kind === "echo") {
            Roost.commands.run("fixture.echo", {a: 1}, cb)
        } else if (kind === "get") {
            Roost.store.get("fixture", cb)
        }
    }
    function sub(cb) { Roost.bus.subscribe("fixture.event", root, cb) }
}
)";

// The shell: callbacks that outlive any panel, and a publisher.
const QByteArray SHELL = R"(
import QtQml
import Roost.Api
QtObject {
    function callbackFor(t) {
        return function (e, r) { roostProbe.Hit(t, JSON.stringify(r)) }
    }
    function getDoc(t) { Roost.store.get("a-one", callbackFor(t)) }
    function publish() {
        Roost.bus.publish("fixture.event", {a: 1}, callbackFor("published"))
    }
}
)";

// The files of a QML-only module `Thing` under `root` (a path relative to the
// plugin directory, the plugin's `qmlImportPath`): one type whose `label` tells
// which copy of the module a panel got.
QMap<QString, QByteArray> ThingModule(const QString& root,
                                      const QString& label) {
    return {
        {root + "/Thing/qmldir", "module Thing\nThing 1.0 Thing.qml\n"},
        {root + "/Thing/Thing.qml",
         QStringLiteral("import QtQuick\nItem { property string label: "
                        "\"%1\" }\n")
             .arg(label)
             .toUtf8()},
    };
}

// A panel whose root is a Thing of the module; reports the label it got.
const QByteArray THING_PANEL = R"(
import QtQuick
import Thing
Thing {
    Component.onCompleted: roostProbe.Hit("label", label)
}
)";

QMap<QString, QByteArray> Joined(QMap<QString, QByteArray> first,
                                 const QMap<QString, QByteArray>& second) {
    first.insert(second);
    return first;
}

QJsonObject Panel(const QString& id, const QString& qml) {
    return {{"id", id}, {"qml", qml}};
}

QJsonObject PanelWithPorts(const QString& id, const QString& qml,
                           const QJsonObject& inputs,
                           const QJsonObject& outputs) {
    return {{"id", id}, {"qml", qml}, {"inputs", inputs}, {"outputs", outputs}};
}

}  // namespace

class PluginLoaderTest : public QObject {
    Q_OBJECT

private:
    // Plugins first (Daemon_->AddQmlPlugin), then this.
    // The loader is made after the list of plugins is loaded (its constructor
    // reads it) or, with `listLoaded` false, before the connection is even
    // asked for. The list may then arrive before Boot returns (inside Connect,
    // if roostd answers first); LoaderTypes_ and LoaderProblems_ see every
    // change of the loader's types and problems from the start.
    bool Boot(bool listLoaded = true) {
        // Names the step that failed and what the Api saw until then, so that
        // a failure in CI says whether a start failed or a wait ran out, and
        // whether the connection never came or the list never did.
        QElapsedTimer elapsed;
        elapsed.start();
        // Shared with the slots, which outlive Boot as long as the Api does.
        const auto seen = std::make_shared<QStringList>();
        const auto fail = [&](const char* step) {
            qWarning().noquote()
                << "Boot failed at" << step << "after" << elapsed.elapsed()
                << "ms; roostd running:" << Daemon_->IsRunning()
                << "; Api connected:" << (Api_ && Api_->IsConnected())
                << "; plugin list loaded:"
                << (Api_ && Api_->Plugins().IsLoaded()) << "; Api signals:"
                << (seen->isEmpty() ? "none" : seen->join(", "));
            return false;
        };
        if (!Daemon_->IsValid()) {
            return fail("the temporary directory of roostd");
        }
        if (!Daemon_->AddFixturePlugin("fixture", FIXTURE_BACKEND_PATH)) {
            return fail("the fixture plugin");
        }
        if (!Daemon_->Start(ROOSTD_PATH)) {
            return fail(
                "the start of roostd (Start returned false; its warnings "
                "above, if any)");
        }
        Api_ = std::make_unique<api::Api>();
        connect(Api_.get(), &api::Api::Disconnected, this,
                [seen](const QString& reason) {
                    *seen << QStringLiteral("Disconnected(%1)").arg(reason);
                });
        connect(Api_.get(), &api::Api::Refused, this,
                [seen](const roost::rpc::Error& error) {
                    *seen << QStringLiteral("Refused(%1 %2)")
                                 .arg(error.Code)
                                 .arg(error.Message);
                });
        connect(Api_.get(), &api::Api::NoListener, this,
                [seen] { *seen << QStringLiteral("NoListener"); });
        Engine_ = std::make_unique<QQmlEngine>();
        Engine_->globalObject().setProperty("roostProbe",
                                            Engine_->newQObject(&Probe_));
        QJSEngine::setObjectOwnership(&Probe_, QJSEngine::CppOwnership);
        Singleton_ =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        if (Singleton_ == nullptr) {
            return fail("the Roost singleton");
        }
        Singleton_->setApi(*Api_);
        QQmlComponent shell(Engine_.get());
        shell.setData(SHELL, QUrl());
        Shell_ = shell.create();
        if (Shell_ == nullptr) {
            qWarning("%s", qPrintable(shell.errorString()));
            return fail("the shell component");
        }
        QJSEngine::setObjectOwnership(Shell_, QJSEngine::CppOwnership);
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        if (!listLoaded) {
            Loader_ = std::make_unique<PluginLoader>(*Engine_, Api_->Plugins(),
                                                     *Singleton_);
            if (!Loader_->PanelTypes().isEmpty()) {
                return fail("the check that the loader starts without types");
            }
            LoaderTypes_ = std::make_unique<QSignalSpy>(
                Loader_.get(), &PluginLoader::PanelTypesChanged);
            LoaderProblems_ = std::make_unique<QSignalSpy>(
                Loader_.get(), &PluginLoader::ProblemsChanged);
            Api_->Connect(Daemon_->SocketPath());
            return true;
        }
        Api_->Connect(Daemon_->SocketPath());
        // Connected may come before the wait: the answer of hello can be there
        // before Api::Connect returns, and then the signal is said inside it.
        if (connected.isEmpty() && !connected.wait(TIMEOUT_MS)) {
            return fail("the Connected signal of the Api");
        }
        *seen << QStringLiteral("Connected at %1 ms").arg(elapsed.elapsed());
        if (!QTest::qWaitFor([&] { return Api_->Plugins().IsLoaded(); },
                             TIMEOUT_MS)) {
            return fail("the plugin list of the Api");
        }
        Loader_ = std::make_unique<PluginLoader>(*Engine_, Api_->Plugins(),
                                                 *Singleton_);
        return true;
    }

    // The plugin dir of a test plugin: two panels of the same plugin, one
    // good, one broken in the way the test says.
    bool AddPlugin(const QString& id, const QJsonArray& panels,
                   const QMap<QString, QByteArray>& files,
                   const QString& version = "0.1.0",
                   const QJsonValue& native = QJsonValue::Undefined) {
        return Daemon_->AddQmlPlugin(id, panels, files, version, native);
    }

    // After a restart of roostd, until the window has the new list.
    bool RestartRoostd() {
        QSignalSpy changed(&Api_->Plugins(), &api::PluginService::Changed);
        if (!Daemon_->Stop() || !Daemon_->Start(ROOSTD_PATH)) {
            return false;
        }
        return changed.wait(TIMEOUT_MS * 2) || changed.count() > 0;
    }

    // roostd stopped, `edit` run on the plugin directories, roostd started
    // again.
    template <typename TEdit>
    bool RestartWith(TEdit&& edit) {
        return Daemon_->Stop() && edit() && Daemon_->Start(ROOSTD_PATH);
    }

    // The document of plugin a-one as roostd gives it, asked of the shell.
    QString ReadDoc() {
        const int before = Probe_.Count("doc");
        if (!QMetaObject::invokeMethod(Shell_, "getDoc",
                                       Q_ARG(QVariant, QString("doc")))) {
            return "no getDoc";
        }
        if (!QTest::qWaitFor([&] { return Probe_.Count("doc") > before; },
                             TIMEOUT_MS)) {
            return "no answer";
        }
        return Probe_.Last.value("doc");
    }

    QVariant CallbackOf(const char* tag) {
        QVariant callback;
        if (!QMetaObject::invokeMethod(Shell_, "callbackFor",
                                       Q_RETURN_ARG(QVariant, callback),
                                       Q_ARG(QVariant, QString(tag)))) {
            qWarning("callbackFor is not callable");
        }
        return callback;
    }

    static bool Invoke(QObject* object, const char* function, const QVariant& a,
                       const QVariant& b) {
        return QMetaObject::invokeMethod(object, function, Q_ARG(QVariant, a),
                                         Q_ARG(QVariant, b));
    }

    // A bounded wait, for what must not happen after a restart of roostd or the
    // destruction of a panel and shows no event to wait on.
    static void Settle() { QTest::qWait(300); }

    // A warning of the loader, for each problem the test expects.
    static void ExpectWarnings(int count) {
        for (int i = 0; i < count; ++i) {
            QTest::ignoreMessage(
                QtWarningMsg,
                QRegularExpression("^roost: plugin [a-z-]+, panel .*"));
        }
    }

    std::unique_ptr<roost::testing::RoostdProcess> Daemon_;
    std::unique_ptr<api::Api> Api_;
    Probe Probe_;
    std::unique_ptr<QQmlEngine> Engine_;
    RoostSingleton* Singleton_ = nullptr;
    QPointer<QObject> Shell_;
    std::unique_ptr<PluginLoader> Loader_;
    // Set by Boot(false): the changes of Loader_'s types since before Connect.
    std::unique_ptr<QSignalSpy> LoaderTypes_;
    // The same for the loader's ProblemsChanged.
    std::unique_ptr<QSignalSpy> LoaderProblems_;

private slots:
    void init() {
        Probe_.Counts.clear();
        Probe_.Last.clear();
        Daemon_ = std::make_unique<roost::testing::RoostdProcess>();
    }

    void cleanup() {
        // Before the engine, as documented; the Api outlives the engine.
        LoaderProblems_.reset();
        LoaderTypes_.reset();
        Loader_.reset();
        delete Shell_.data();
        Engine_.reset();
        Singleton_ = nullptr;
        Api_.reset();
        Daemon_.reset();
    }

    void TypesOfTwoPluginsLoadAndPanelsAreCreated() {
        QVERIFY(AddPlugin(
            "a-one", {Panel("a", "one-a.qml"), Panel("plain", "plain.qml")},
            {{"one-a.qml", ItemPanel("one-a")}, {"plain.qml", PLAIN_PANEL}}));
        QVERIFY(AddPlugin("b-two", {Panel("a", "qml/two-a.qml")},
                          {{"qml/two-a.qml", ItemPanel("two-a")}}));
        QVERIFY(Boot());
        QCOMPARE(Loader_->PanelTypes(),
                 (QStringList{"a-one.a", "a-one.plain", "b-two.a"}));
        QVERIFY(Loader_->Problems().isEmpty());

        QQuickItem* one = Loader_->CreatePanel("a-one.a");
        QVERIFY(one != nullptr);
        QCOMPARE(one->property("pluginId").toString(), QString("a-one"));
        // Set before Component.onCompleted.
        QCOMPARE(Probe_.Last.value("completed:one-a"), QString("a-one"));
        QQuickItem* two = Loader_->CreatePanel("b-two.a");
        QVERIFY(two != nullptr);
        QCOMPARE(Probe_.Last.value("completed:two-a"), QString("b-two"));
        QQuickItem* second = Loader_->CreatePanel("a-one.a");
        QVERIFY(second != nullptr && second != one);
        // Each instance has its own context, below the context of its plugin.
        const auto panelContext = [](QQuickItem* item) {
            return QQmlEngine::contextForObject(item)->parentContext();
        };
        QVERIFY(panelContext(one) != panelContext(second));
        QCOMPARE(panelContext(one)->parentContext(),
                 panelContext(second)->parentContext());
        QVERIFY(panelContext(one)->parentContext() !=
                panelContext(two)->parentContext());

        // No pluginId property: the panel still works.
        QQuickItem* plain = Loader_->CreatePanel("a-one.plain");
        if (plain == nullptr) {
            QFAIL("the panel without pluginId was not created");
        }
        QVERIFY(!plain->property("pluginId").isValid());
        QCOMPARE(Probe_.Last.value("completed:plain"), QString("ran"));

        // Unknown type: nullptr, and no problem for it.
        QVERIFY(Loader_->CreatePanel("no.such") == nullptr);
        QVERIFY(Loader_->Problems().isEmpty());
        // The instances belong to the loader, not to the JS engine.
        QCOMPARE(QJSEngine::objectOwnership(one), QJSEngine::CppOwnership);
    }

    void ALoaderMadeBeforeTheListArrivesLoadsOnChanged() {
        QVERIFY(AddPlugin("a-one", {Panel("p", "p.qml")},
                          {{"p.qml", ItemPanel("p")}}));
        QVERIFY(Boot(false));
        QTRY_COMPARE_WITH_TIMEOUT(LoaderTypes_->count(), 1, TIMEOUT_MS);
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"a-one.p"}));
    }

    void ABrokenPanelDisablesOnlyItself_data() {
        QTest::addColumn<QJsonValue>("entry");
        QTest::addColumn<QString>("panelId");
        QTest::addColumn<QString>("message");
        const auto row = [](const char* name, const QJsonValue& entry,
                            const QString& panelId, const QString& message) {
            QTest::newRow(name) << entry << panelId << message;
        };
        row("entry-not-an-object", "text", "", "must be an object");
        row("id-missing", QJsonObject{{"qml", "good.qml"}}, "",
            "\"id\" must be a non-empty string");
        row("id-not-a-string", QJsonObject{{"id", 5}, {"qml", "good.qml"}}, "",
            "\"id\" must be a non-empty string");
        row("id-empty", QJsonObject{{"id", ""}, {"qml", "good.qml"}}, "",
            "\"id\" must be a non-empty string");
        // Not a name relative to the plugin: the problem names the entry as
        // written.
        row("id-with-dot", Panel("x.y", "good.qml"), "x.y", "without '.'");
        row("id-with-uppercase", Panel("Up", "good.qml"), "Up",
            "lowercase letters");
        row("qml-missing", QJsonObject{{"id", "p"}}, "a-bad.p",
            "\"qml\" must be a non-empty string");
        row("qml-not-a-string", QJsonObject{{"id", "p"}, {"qml", 5}}, "a-bad.p",
            "\"qml\" must be a non-empty string");
        row("qml-absolute", Panel("p", "/tmp/panel.qml"), "a-bad.p",
            "inside the plugin directory");
        row("qml-escapes", Panel("p", "../escape.qml"), "a-bad.p",
            "inside the plugin directory");
        row("qml-escapes-deep", Panel("p", "qml/../../escape.qml"), "a-bad.p",
            "inside the plugin directory");
        row("inputs-not-an-object",
            QJsonObject{{"id", "p"}, {"qml", "good.qml"}, {"inputs", "x"}},
            "a-bad.p", "\"inputs\" must be an object");
        row("outputs-not-an-object",
            QJsonObject{{"id", "p"}, {"qml", "good.qml"}, {"outputs", 1}},
            "a-bad.p", "\"outputs\" must be an object");
        row("file-missing", Panel("p", "missing.qml"), "a-bad.p",
            "does not exist");
        row("syntax-error", Panel("p", "syntax.qml"), "a-bad.p", "syntax.qml");
        row("unknown-type", Panel("p", "unknown.qml"), "a-bad.p", "NoSuchType");
    }

    void ABrokenPanelDisablesOnlyItself() {
        QFETCH(QJsonValue, entry);
        QFETCH(QString, panelId);
        QFETCH(QString, message);
        QVERIFY(AddPlugin("a-bad", {entry, Panel("good", "good.qml")},
                          {{"good.qml", ItemPanel("good")},
                           {"syntax.qml", SYNTAX_ERROR_PANEL},
                           {"unknown.qml", UNKNOWN_TYPE_PANEL}}));
        QVERIFY(AddPlugin("b-other", {Panel("other", "other.qml")},
                          {{"other.qml", ItemPanel("other")}}));
        ExpectWarnings(1);
        QVERIFY(Boot());
        // The sibling of the same plugin and the other plugin load.
        QCOMPARE(Loader_->PanelTypes(),
                 (QStringList{"a-bad.good", "b-other.other"}));
        QVERIFY(Loader_->CreatePanel("a-bad.good") != nullptr);
        QVERIFY(Loader_->CreatePanel("b-other.other") != nullptr);
        QCOMPARE(Loader_->Problems().size(), 1);
        const auto problem = Loader_->Problems().first();
        QCOMPARE(problem.Plugin, QString("a-bad"));
        QCOMPARE(problem.Panel, panelId);
        QVERIFY2(problem.Message.contains(message),
                 qPrintable(problem.Message));
        if (!panelId.isEmpty()) {
            QVERIFY(Loader_->CreatePanel(panelId) == nullptr);
            QCOMPARE(Loader_->Problems().size(), 1);
        }
    }

    void APanelNameIsRelativeToItsPluginAndTheTypeIsPluginDotName() {
        QVERIFY(AddPlugin("foo", {Panel("hello", "hello.qml")},
                          {{"hello.qml", ItemPanel("hello")}}));
        QVERIFY(Boot());
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"foo.hello"}));
        QVERIFY(Loader_->Problems().isEmpty());
        QVERIFY(Loader_->CreatePanel("hello") == nullptr);
        QQuickItem* item = Loader_->CreatePanel("foo.hello");
        QVERIFY(item != nullptr);
        QCOMPARE(item->property("pluginId").toString(), QString("foo"));
        const auto info = Loader_->Info("foo.hello");
        QVERIFY(info.has_value());
        QCOMPARE(info->Plugin, QString("foo"));
        QVERIFY(!Loader_->Info("hello").has_value());
    }

    void ADottedPanelNameIsAProblemAndOnlyThatPanelIsNotCreated() {
        QVERIFY(AddPlugin("foo",
                          {Panel("a.b", "a.qml"), Panel("ok", "ok.qml"),
                           Panel("foo.c", "c.qml")},
                          {{"a.qml", ItemPanel("a")},
                           {"ok.qml", ItemPanel("ok")},
                           {"c.qml", ItemPanel("c")}}));
        ExpectWarnings(2);
        QVERIFY(Boot());
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"foo.ok"}));
        QVERIFY(Loader_->CreatePanel("foo.ok") != nullptr);
        for (const QString& id : {"a.b", "foo.a.b", "foo.foo.c", "foo.c"}) {
            QVERIFY2(Loader_->CreatePanel(id) == nullptr, qPrintable(id));
        }
        QCOMPARE(Loader_->Problems().size(), 2);
        QCOMPARE(Loader_->Problems().at(0).Plugin, QString("foo"));
        QCOMPARE(Loader_->Problems().at(0).Panel, QString("a.b"));
        QCOMPARE(Loader_->Problems().at(1).Panel, QString("foo.c"));
        QVERIFY2(Loader_->Problems().at(0).Message.contains("without '.'"),
                 qPrintable(Loader_->Problems().at(0).Message));
    }

    void TwoPluginsWithTheSamePanelNameBothLoad() {
        QVERIFY(AddPlugin("a", {Panel("view", "view.qml")},
                          {{"view.qml", ItemPanel("from-a")}}));
        QVERIFY(AddPlugin("b", {Panel("view", "view.qml")},
                          {{"view.qml", ItemPanel("from-b")}}));
        QVERIFY(Boot());
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"a.view", "b.view"}));
        QVERIFY(Loader_->Problems().isEmpty());
        QQuickItem* first = Loader_->CreatePanel("a.view");
        QQuickItem* second = Loader_->CreatePanel("b.view");
        QVERIFY(first != nullptr);
        QVERIFY(second != nullptr);
        QCOMPARE(first->property("content").toString(), QString("from-a"));
        QCOMPARE(first->property("pluginId").toString(), QString("a"));
        QCOMPARE(second->property("content").toString(), QString("from-b"));
        QCOMPARE(second->property("pluginId").toString(), QString("b"));
    }

    void ADottedPanelNameCannotMakeTheTypeOfAnotherPlugin() {
        // Plugin "a" with the name "b.c" and plugin "a.b" with the name "c"
        // would both be "a.b.c" if the dot were allowed in a name.
        QVERIFY(AddPlugin("a", {Panel("b.c", "bc.qml")},
                          {{"bc.qml", ItemPanel("from-a")}}));
        QVERIFY(AddPlugin("a.b", {Panel("c", "c.qml")},
                          {{"c.qml", ItemPanel("from-a-b")}}));
        ExpectWarnings(1);
        QVERIFY(Boot());
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"a.b.c"}));
        QCOMPARE(Loader_->Problems().size(), 1);
        QCOMPARE(Loader_->Problems().at(0).Plugin, QString("a"));
        QCOMPARE(Loader_->Problems().at(0).Panel, QString("b.c"));
        QQuickItem* item = Loader_->CreatePanel("a.b.c");
        QVERIFY(item != nullptr);
        QCOMPARE(item->property("content").toString(), QString("from-a-b"));
        QCOMPARE(item->property("pluginId").toString(), QString("a.b"));
    }

    void ASecondDirectoryWithTheSamePluginIdIsNotLoaded() {
        QVERIFY(AddPlugin("dup", {Panel("view", "view.qml")},
                          {{"view.qml", ItemPanel("first")}}));
        // A second directory whose manifest has the same id.
        const QDir other(Daemon_->PluginsPath() + "/dup-again");
        QVERIFY(other.mkpath("."));
        QFile qml(other.filePath("view.qml"));
        QVERIFY(qml.open(QIODevice::WriteOnly));
        QVERIFY(qml.write(ItemPanel("second")) > 0);
        qml.close();
        QFile manifest(other.filePath("plugin.json"));
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        QVERIFY(
            manifest.write(
                QJsonDocument(
                    QJsonObject{
                        {"id", "dup"},
                        {"version", "0.1.0"},
                        {"api", "0"},
                        {"contributes",
                         QJsonObject{{"panels",
                                      QJsonArray{Panel("view", "view.qml")}}}}})
                    .toJson()) > 0);
        manifest.close();
        QVERIFY(Boot());
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"dup.view"}));
        QVERIFY(Loader_->Problems().isEmpty());
        QCOMPARE(
            Loader_->CreatePanel("dup.view")->property("content").toString(),
            QString("first"));
    }

    void ADuplicatePanelIdInOnePluginKeepsTheFirst() {
        QVERIFY(AddPlugin(
            "a-one", {Panel("dup", "one.qml"), Panel("dup", "two.qml")},
            {{"one.qml", ItemPanel("one")}, {"two.qml", ItemPanel("two")}}));
        ExpectWarnings(1);
        QVERIFY(Boot());
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"a-one.dup"}));
        QCOMPARE(
            Loader_->CreatePanel("a-one.dup")->property("content").toString(),
            QString("one"));
        QCOMPARE(Loader_->Problems().size(), 1);
    }

    void ARootThatIsNotAnItemFailsOnceAndDisablesTheType() {
        QVERIFY(AddPlugin(
            "a-one", {Panel("object", "object.qml"), Panel("good", "good.qml")},
            {{"object.qml", OBJECT_PANEL}, {"good.qml", ItemPanel("good")}}));
        QVERIFY(Boot());
        QCOMPARE(Loader_->PanelTypes(),
                 (QStringList{"a-one.object", "a-one.good"}));
        QSignalSpy types(Loader_.get(), &PluginLoader::PanelTypesChanged);
        QSignalSpy problems(Loader_.get(), &PluginLoader::ProblemsChanged);
        ExpectWarnings(1);
        QVERIFY(Loader_->CreatePanel("a-one.object") == nullptr);
        QCOMPARE(Loader_->Problems().size(), 1);
        QCOMPARE(Loader_->Problems().first().Plugin, QString("a-one"));
        QCOMPARE(Loader_->Problems().first().Panel, QString("a-one.object"));
        QVERIFY2(Loader_->Problems().first().Message.contains("Item"),
                 qPrintable(Loader_->Problems().first().Message));
        QCOMPARE(types.count(), 1);
        QCOMPARE(problems.count(), 1);
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"a-one.good"}));
        // Disabled: the second try fails quietly.
        QVERIFY(Loader_->CreatePanel("a-one.object") == nullptr);
        QCOMPARE(Loader_->Problems().size(), 1);
        QCOMPARE(types.count(), 1);
        QCOMPARE(problems.count(), 1);
        // The sibling is fine.
        QVERIFY(Loader_->CreatePanel("a-one.good") != nullptr);
    }

    void AnUnchangedPluginIsNotTouched_data() {
        QTest::addColumn<QString>("how");
        QTest::newRow("fetch") << "fetch";
        QTest::newRow("restart") << "restart";
        // roostd sees no change of version or contributes: not reloaded, and
        // the window does not look at the file.
        QTest::newRow("restart-file-edited") << "restart-file-edited";
    }

    void AnUnchangedPluginIsNotTouched() {
        QFETCH(QString, how);
        const QJsonArray panels{Panel("p", "p.qml")};
        QVERIFY(AddPlugin("a-one", panels, {{"p.qml", ItemPanel("v1")}}));
        QVERIFY(Boot());
        QQuickItem* item = Loader_->CreatePanel("a-one.p");
        QVERIFY(item != nullptr);
        QPointer<QQuickItem> guard = item;
        QSignalSpy destroyed(Loader_.get(),
                             &PluginLoader::PanelAboutToBeDestroyed);
        QSignalSpy types(Loader_.get(), &PluginLoader::PanelTypesChanged);
        QSignalSpy problems(Loader_.get(), &PluginLoader::ProblemsChanged);
        if (how == "fetch") {
            QSignalSpy changed(&Api_->Plugins(), &api::PluginService::Changed);
            Api_->Plugins().Fetch();
            // The answer may be there before Fetch returns: look first.
            QVERIFY(!changed.isEmpty() || changed.wait(TIMEOUT_MS));
        } else {
            if (how == "restart-file-edited") {
                QVERIFY(
                    AddPlugin("a-one", panels, {{"p.qml", ItemPanel("v2")}}));
            }
            QVERIFY(RestartRoostd());
        }
        // The fetch is answered when Changed arrives, and the loader has
        // processed it; after a restart a late second list may still come.
        if (how != "fetch") {
            Settle();
        }
        QCOMPARE(destroyed.count(), 0);
        QCOMPARE(types.count(), 0);
        QCOMPARE(problems.count(), 0);
        QVERIFY(guard == item);
        QCOMPARE(guard->property("content").toString(), QString("v1"));
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"a-one.p"}));
        // New instances come from the same component: still v1 content.
        QCOMPARE(
            Loader_->CreatePanel("a-one.p")->property("content").toString(),
            QString("v1"));
    }

    void AChangedPluginIsReloaded_data() {
        QTest::addColumn<QString>("what");
        QTest::newRow("version") << "version";
        QTest::newRow("contributes") << "contributes";
    }

    void AChangedPluginIsReloaded() {
        QFETCH(QString, what);
        QVERIFY(AddPlugin("a-one", {Panel("p", "p.qml")},
                          {{"p.qml", ItemPanel("v1")}}));
        QVERIFY(AddPlugin("b-two", {Panel("q", "q.qml")},
                          {{"q.qml", ItemPanel("q1")}}));
        QVERIFY(Boot());
        QQuickItem* old = Loader_->CreatePanel("a-one.p");
        QQuickItem* bystander = Loader_->CreatePanel("b-two.q");
        QVERIFY(old != nullptr && bystander != nullptr);
        QPointer<QQuickItem> oldGuard = old;
        QPointer<QQuickItem> bystanderGuard = bystander;
        QSignalSpy types(Loader_.get(), &PluginLoader::PanelTypesChanged);
        QList<QQuickItem*> torn;
        QStringList contentSeen;
        // The context object ends the connection with the test function: the
        // lambda must not outlive the locals.
        QObject scope;
        connect(
            Loader_.get(), &PluginLoader::PanelAboutToBeDestroyed, &scope,
            [&](QQuickItem* item) {
                torn.append(item);
                // Alive: its properties can be read.
                contentSeen.append(item->property("content").toString());
            },
            Qt::DirectConnection);

        QCOMPARE(Daemon_->Stop(), true);
        QJsonArray panels{Panel("p", "p.qml")};
        QString version = "0.1.0";
        if (what == "version") {
            version = "0.2.0";
        } else {
            panels.append(Panel("p2", "p.qml"));
        }
        QVERIFY(
            AddPlugin("a-one", panels, {{"p.qml", ItemPanel("v2")}}, version));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QTRY_COMPARE_WITH_TIMEOUT(types.count(), 1, TIMEOUT_MS * 2);

        QCOMPARE(types.count(), 1);
        QCOMPARE(torn.size(), 1);
        QCOMPARE(torn.first(), old);
        QCOMPARE(contentSeen, (QStringList{"v1"}));
        QVERIFY(oldGuard.isNull());
        QVERIFY(!bystanderGuard.isNull());
        // The edited file is read from disk again.
        QQuickItem* fresh = Loader_->CreatePanel("a-one.p");
        QVERIFY(fresh != nullptr);
        QCOMPARE(fresh->property("content").toString(), QString("v2"));
        QCOMPARE(
            Loader_->CreatePanel("b-two.q")->property("content").toString(),
            QString("q1"));
        QCOMPARE(Loader_->PanelTypes().contains("a-one.p2"),
                 what == "contributes");
    }

    void ARemovedPluginIsUnloaded() {
        QVERIFY(AddPlugin("a-one", {Panel("p", "p.qml")},
                          {{"p.qml", ItemPanel("p")}}));
        QVERIFY(AddPlugin("b-two",
                          {Panel("q", "q.qml"), Panel("broken", "missing.qml")},
                          {{"q.qml", ItemPanel("q")}}));
        ExpectWarnings(1);
        QVERIFY(Boot());
        QQuickItem* kept = Loader_->CreatePanel("a-one.p");
        QQuickItem* gone = Loader_->CreatePanel("b-two.q");
        QQuickItem* gone2 = Loader_->CreatePanel("b-two.q");
        QVERIFY(kept != nullptr && gone != nullptr && gone2 != nullptr);
        QPointer<QQuickItem> keptGuard = kept;
        QPointer<QQuickItem> goneGuard = gone;
        QPointer<QQuickItem> gone2Guard = gone2;
        QCOMPARE(Loader_->Problems().size(), 1);
        QSignalSpy destroyed(Loader_.get(),
                             &PluginLoader::PanelAboutToBeDestroyed);
        QSignalSpy types(Loader_.get(), &PluginLoader::PanelTypesChanged);
        QSignalSpy problems(Loader_.get(), &PluginLoader::ProblemsChanged);

        QVERIFY(Daemon_->Stop());
        QVERIFY(QDir(Daemon_->PluginsPath() + "/b-two").removeRecursively());
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QTRY_COMPARE_WITH_TIMEOUT(types.count(), 1, TIMEOUT_MS * 2);

        QCOMPARE(destroyed.count(), 2);
        QVERIFY(goneGuard.isNull() && gone2Guard.isNull());
        QVERIFY(!keptGuard.isNull());
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"a-one.p"}));
        QVERIFY(Loader_->Problems().isEmpty());
        QCOMPARE(problems.count(), 1);
        QVERIFY(Loader_->CreatePanel("b-two.q") == nullptr);
        QCOMPARE(types.count(), 1);
    }

    void ANewPluginIsLoadedWhenItAppears() {
        QVERIFY(AddPlugin("a-one", {Panel("p", "p.qml")},
                          {{"p.qml", ItemPanel("p")}}));
        QVERIFY(Boot());
        QQuickItem* item = Loader_->CreatePanel("a-one.p");
        QPointer<QQuickItem> guard = item;
        QSignalSpy types(Loader_.get(), &PluginLoader::PanelTypesChanged);
        QVERIFY(Daemon_->Stop());
        QVERIFY(AddPlugin("b-two", {Panel("q", "q.qml")},
                          {{"q.qml", ItemPanel("q")}}));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QTRY_COMPARE_WITH_TIMEOUT(types.count(), 1, TIMEOUT_MS * 2);
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"a-one.p", "b-two.q"}));
        QVERIFY(!guard.isNull());
    }

    void DestroyPanelTearsDownInOrder() {
        QVERIFY(AddPlugin("a-one", {Panel("p", "p.qml")},
                          {{"p.qml", ItemPanel("p")}}));
        QVERIFY(Boot());
        QQuickItem* item = Loader_->CreatePanel("a-one.p");
        QVERIFY(item != nullptr);
        QPointer<QQuickItem> guard = item;
        QPointer<QQmlContext> context =
            QQmlEngine::contextForObject(item)->parentContext();
        QVERIFY(context != nullptr);
        QObject* owner = context->findChild<QObject*>();
        QVERIFY(owner != nullptr);
        QPointer<QObject> ownerGuard = owner;
        QQuickItem* seen = nullptr;
        bool contextAlive = false;
        bool ownerAlive = false;
        int emitted = 0;
        QObject scope;
        connect(
            Loader_.get(), &PluginLoader::PanelAboutToBeDestroyed, &scope,
            [&](QQuickItem* panel) {
                ++emitted;
                seen = panel;
                contextAlive = !context.isNull();
                ownerAlive = !ownerGuard.isNull();
                // A slot that comes back does no harm.
                Loader_->DestroyPanel(panel);
            },
            Qt::DirectConnection);
        Loader_->DestroyPanel(nullptr);
        Loader_->DestroyPanel(item);
        QCOMPARE(emitted, 1);
        QCOMPARE(seen, item);
        QVERIFY(contextAlive && ownerAlive);
        QVERIFY(guard.isNull());
        QVERIFY(ownerGuard.isNull());
        QVERIFY(context.isNull());
        // Not an instance of the loader any more: nothing happens.
        Loader_->DestroyPanel(item);
        QCOMPARE(emitted, 1);
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"a-one.p"}));
    }

    void ACallOfADestroyedPanelNeverAnswers_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("echo") << "echo";
        QTest::newRow("get") << "get";
    }

    void ACallOfADestroyedPanelNeverAnswers() {
        QFETCH(QString, kind);
        QVERIFY(AddPlugin("a-one", {Panel("calls", "calls.qml")},
                          {{"calls.qml", CALLS_PANEL}}));
        QVERIFY(Boot());
        QQuickItem* panel = Loader_->CreatePanel("a-one.calls");
        QQuickItem* live = Loader_->CreatePanel("a-one.calls");
        QVERIFY(panel != nullptr);
        QVERIFY(live != nullptr);
        // The callback belongs to the shell and outlives the panel: only the
        // owner of the call can stop it.
        QVERIFY(Invoke(panel, "run", kind, CallbackOf("panel")));
        // The control, asked after it: roostd answers in the order it was
        // asked, so when the control is answered the call of the panel has been
        // too.
        QVERIFY(Invoke(live, "run", kind, CallbackOf("control")));
        Loader_->DestroyPanel(panel);
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("control"), 1, TIMEOUT_MS);
        QCOMPARE(Probe_.Count("panel"), 0);
    }

    void ASlotOfPanelAboutToBeDestroyedCannotCreateInTheUnloadingPlugin() {
        QVERIFY(
            AddPlugin("a-one", {Panel("p", "p.qml")}, {{"p.qml", LIFE_PANEL}}));
        QVERIFY(
            AddPlugin("b-two", {Panel("q", "q.qml")}, {{"q.qml", LIFE_PANEL}}));
        QVERIFY(Boot());
        QQuickItem* first = Loader_->CreatePanel("a-one.p");
        QQuickItem* second = Loader_->CreatePanel("a-one.p");
        QVERIFY(first != nullptr);
        QVERIFY(second != nullptr);
        QSignalSpy types(Loader_.get(), &PluginLoader::PanelTypesChanged);
        QObject scope;
        int slotCalls = 0;
        QList<QQuickItem*> offered;
        QList<QQuickItem*> bystanders;
        connect(
            Loader_.get(), &PluginLoader::PanelAboutToBeDestroyed, &scope,
            [&](QQuickItem*) {
                ++slotCalls;
                offered.append(Loader_->CreatePanel("a-one.p"));
                bystanders.append(Loader_->CreatePanel("b-two.q"));
            },
            Qt::DirectConnection);

        QVERIFY(RestartWith([&] {
            return QDir(Daemon_->PluginsPath() + "/a-one").removeRecursively();
        }));
        QTRY_COMPARE_WITH_TIMEOUT(types.count(), 1, TIMEOUT_MS * 2);

        QCOMPARE(slotCalls, 2);
        for (QQuickItem* item : offered) {
            QVERIFY(item == nullptr);
        }
        // Another plugin is not unloading: its types still work.
        for (QQuickItem* item : bystanders) {
            QVERIFY(item != nullptr);
        }
        QCOMPARE(Probe_.Count("completed:life"), 2 + 2);
        QCOMPARE(Probe_.Count("destroyed:life"), 2);
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"b-two.q"}));
    }

    void ADestroyedLoaderTearsItsPanelsDownWithTheItemAlive() {
        QVERIFY(AddPlugin("a-one", {Panel("p", "p.qml")},
                          {{"p.qml", ItemPanel("v1")}}));
        QVERIFY(Boot());
        QQuickItem* item = Loader_->CreatePanel("a-one.p");
        QVERIFY(item != nullptr);
        QPointer<QQuickItem> guard = item;
        QObject scope;
        int emitted = 0;
        QString contentSeen;
        QQuickItem* seen = nullptr;
        connect(
            Loader_.get(), &PluginLoader::PanelAboutToBeDestroyed, &scope,
            [&](QQuickItem* panel) {
                ++emitted;
                seen = panel;
                contentSeen = panel->property("content").toString();
            },
            Qt::DirectConnection);
        Loader_.reset();
        QCOMPARE(emitted, 1);
        QCOMPARE(seen, item);
        QCOMPARE(contentSeen, QString("v1"));
        QVERIFY(guard.isNull());
    }

    void APanelWithARequiredPluginIdIsCreated() {
        QVERIFY(AddPlugin("a-one", {Panel("p", "p.qml")},
                          {{"p.qml", REQUIRED_PANEL}}));
        QVERIFY(Boot());
        QQuickItem* item = Loader_->CreatePanel("a-one.p");
        QVERIFY2(item != nullptr, "the required pluginId was not satisfied");
        QCOMPARE(item->property("pluginId").toString(), QString("a-one"));
        QCOMPARE(Probe_.Last.value("completed:required"), QString("a-one"));
        QVERIFY(Loader_->Problems().isEmpty());
    }

    void APluginThatIsRemovedAndComesBackIsReadFromDisk() {
        QVERIFY(AddPlugin("a-one", {Panel("p", "p.qml")},
                          {{"p.qml", ItemPanel("v1")}}));
        QVERIFY(Boot());
        QQuickItem* old = Loader_->CreatePanel("a-one.p");
        QVERIFY(old != nullptr);
        QCOMPARE(old->property("content").toString(), QString("v1"));
        Loader_->DestroyPanel(old);
        QSignalSpy types(Loader_.get(), &PluginLoader::PanelTypesChanged);

        // One list without the plugin.
        QVERIFY(RestartWith([&] {
            return QDir(Daemon_->PluginsPath() + "/a-one").removeRecursively();
        }));
        QTRY_COMPARE_WITH_TIMEOUT(types.count(), 1, TIMEOUT_MS * 2);
        QVERIFY(Loader_->PanelTypes().isEmpty());

        // A later one with it again, edited and in a new version.
        QVERIFY(RestartWith([&] {
            return AddPlugin("a-one", {Panel("p", "p.qml")},
                             {{"p.qml", ItemPanel("v2")}}, "0.2.0");
        }));
        QTRY_COMPARE_WITH_TIMEOUT(types.count(), 2, TIMEOUT_MS * 2);
        QQuickItem* fresh = Loader_->CreatePanel("a-one.p");
        QVERIFY(fresh != nullptr);
        QCOMPARE(fresh->property("content").toString(), QString("v2"));
    }

    void ProblemsChangedFiresOnTheFirstLoadAndWhenAReloadClearsThem() {
        QVERIFY(AddPlugin("a-one", {Panel("p", "missing.qml")}, {}));
        ExpectWarnings(1);
        QVERIFY(Boot(false));
        QSignalSpy& problems = *LoaderProblems_;
        QTRY_COMPARE_WITH_TIMEOUT(problems.count(), 1, TIMEOUT_MS);
        QCOMPARE(Loader_->Problems().size(), 1);

        QVERIFY(RestartWith([&] {
            return AddPlugin("a-one", {Panel("p", "p.qml")},
                             {{"p.qml", ItemPanel("p")}}, "0.2.0");
        }));
        QTRY_COMPARE_WITH_TIMEOUT(problems.count(), 2, TIMEOUT_MS * 2);
        QVERIFY(Loader_->Problems().isEmpty());
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"a-one.p"}));
    }

    void InfoGivesTheDeclaredInputsAndOutputsOfACreatableType() {
        QVERIFY(AddPlugin(
            "a-one",
            {PanelWithPorts("typed", "typed.qml", {{"session", "SessionRef"}},
                            {{"selected", "SessionRef"}, {"count", "Number"}}),
             Panel("bare", "plain.qml"), Panel("broken", "broken.qml")},
            {{"typed.qml", PORTS_PANEL},
             {"plain.qml", PLAIN_PANEL},
             {"broken.qml", SYNTAX_ERROR_PANEL}}));
        QVERIFY(Boot());
        const auto typed = Loader_->Info("a-one.typed");
        QVERIFY(typed.has_value());
        QCOMPARE(typed->Plugin, QString("a-one"));
        QCOMPARE(typed->Inputs, (QVariantMap{{"session", "SessionRef"}}));
        QCOMPARE(typed->Outputs, (QVariantMap{{"selected", "SessionRef"},
                                              {"count", "Number"}}));
        const auto bare = Loader_->Info("a-one.bare");
        QVERIFY(bare.has_value());
        QVERIFY(bare->Inputs.isEmpty() && bare->Outputs.isEmpty());
        // A type that cannot be created, and one nobody declared.
        QVERIFY(!Loader_->Info("a-one.broken").has_value());
        QVERIFY(!Loader_->Info("nothing").has_value());
        QVERIFY(Loader_->Problems().size() == 1);
    }

    void ADeclaredNameTheRootLacksIsAProblemAndThePanelIsStillCreated() {
        QVERIFY(AddPlugin(
            "a-one",
            {PanelWithPorts("full", "ports.qml", {{"session", "SessionRef"}},
                            {{"selected", "SessionRef"}}),
             PanelWithPorts("short", "ports.qml",
                            {{"session", "SessionRef"}, {"missingIn", "Text"}},
                            {{"missingOut", "SessionRef"}})},
            {{"ports.qml", PORTS_PANEL}}));
        QVERIFY(Boot());
        QSignalSpy problems(Loader_.get(), &PluginLoader::ProblemsChanged);

        QQuickItem* full = Loader_->CreatePanel("a-one.full");
        QVERIFY(full != nullptr);
        QVERIFY(Loader_->Problems().isEmpty());
        QVERIFY(problems.isEmpty());

        QQuickItem* panel = Loader_->CreatePanel("a-one.short");
        QVERIFY(panel != nullptr);
        QCOMPARE(Loader_->Problems().size(), 2);
        QStringList messages;
        for (const auto& problem : Loader_->Problems()) {
            QCOMPARE(problem.Plugin, QString("a-one"));
            QCOMPARE(problem.Panel, QString("a-one.short"));
            messages.append(problem.Message);
        }
        messages.sort();
        QCOMPARE(messages,
                 (QStringList{"declared input \"missingIn\" is not a property "
                              "of the root",
                              "declared output \"missingOut\" is not a "
                              "property of the root"}));
        QCOMPARE(problems.count(), 1);
        // The panel is usable and the type stays on offer.
        QVERIFY(Loader_->PanelTypes().contains("a-one.short"));
        QVERIFY(Loader_->Info("a-one.short").has_value());

        // A second instance reports the same thing once.
        QVERIFY(Loader_->CreatePanel("a-one.short") != nullptr);
        QCOMPARE(Loader_->Problems().size(), 2);
        QCOMPARE(problems.count(), 1);
    }

    void ADestroyedPanelsFarewellCallIsDroppedBecauseTheOwnerGoesFirst() {
        QVERIFY(AddPlugin("a-one", {Panel("p", "p.qml")},
                          {{"p.qml", FAREWELL_PANEL}}));
        QVERIFY(Boot());
        QQuickItem* panel = Loader_->CreatePanel("a-one.p");
        QVERIFY(panel != nullptr);
        // The control: a call of the live panel reaches roostd.
        QVERIFY(QMetaObject::invokeMethod(panel, "save"));
        QTRY_VERIFY_WITH_TIMEOUT(ReadDoc().contains("live"), TIMEOUT_MS);

        Loader_->DestroyPanel(panel);
        // A late call would reach roostd after a while.
        Settle();
        const QString doc = ReadDoc();
        QVERIFY2(doc.contains("live") && !doc.contains("farewell"),
                 qPrintable(doc));
    }

    void ACallOfALivePanelIsAnswered() {
        QVERIFY(AddPlugin("a-one", {Panel("calls", "calls.qml")},
                          {{"calls.qml", CALLS_PANEL}}));
        QVERIFY(Boot());
        QQuickItem* panel = Loader_->CreatePanel("a-one.calls");
        QVERIFY(panel != nullptr);
        QVERIFY(Invoke(panel, "run", "get", CallbackOf("panel")));
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("panel"), 1, TIMEOUT_MS);
    }

    void ASubscriptionOfADestroyedPanelIsGone() {
        QVERIFY(AddPlugin("a-one", {Panel("calls", "calls.qml")},
                          {{"calls.qml", CALLS_PANEL}}));
        QVERIFY(Boot());
        QQuickItem* panel = Loader_->CreatePanel("a-one.calls");
        QVERIFY(panel != nullptr);
        QVERIFY(QMetaObject::invokeMethod(panel, "sub",
                                          Q_ARG(QVariant, CallbackOf("bus"))));
        QVERIFY(QMetaObject::invokeMethod(Shell_, "publish"));
        // The control: while the panel lives the handler runs.
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("bus"), 1, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("published"), 1, TIMEOUT_MS);
        Loader_->DestroyPanel(panel);
        QVERIFY(QMetaObject::invokeMethod(Shell_, "publish"));
        // roostd sends the event before it answers the publish, on one
        // connection: a handler of the destroyed panel would have run by now.
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("published"), 2, TIMEOUT_MS);
        QCOMPARE(Probe_.Count("bus"), 1);
    }

    void APanelImportsTheModuleOfItsPlugin() {
        QVERIFY(AddPlugin(
            "a-one", {Panel("p", "p.qml")},
            Joined({{"p.qml", THING_PANEL}}, ThingModule("qml", "one")),
            "0.1.0", QJsonObject{{"qmlImportPath", "qml"}}));
        QVERIFY(Boot());
        QVERIFY(Loader_->Problems().isEmpty());
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"a-one.p"}));
        QVERIFY(Loader_->CreatePanel("a-one.p") != nullptr);
        QCOMPARE(Probe_.Last.value("label"), QString("one"));
    }

    void APluginImportPathComesAfterTheEnginesOwn() {
        // Roost.Api is imported before any plugin loads, so a shadow module
        // cannot win here whatever the order: the import path is what is
        // checked. Every path the engine had keeps its place, and the
        // plugin's directory comes after them.
        const QStringList before = QQmlEngine().importPathList();
        const QMap<QString, QByteArray> shadow{
            {"qml/Roost/Api/qmldir",
             "module Roost.Api\nsingleton Roost 1.0 Roost.qml\n"},
            {"qml/Roost/Api/Roost.qml",
             "pragma Singleton\nimport QtQuick\nQtObject { property string "
             "shadow: \"yes\" }\n"},
        };
        const QByteArray panel = R"(
import QtQuick
import Roost.Api
Item {
    Component.onCompleted: roostProbe.Hit("shadow", typeof Roost.shadow)
}
)";
        QVERIFY(AddPlugin("a-one", {Panel("p", "p.qml")},
                          Joined({{"p.qml", panel}}, shadow), "0.1.0",
                          QJsonObject{{"qmlImportPath", "qml"}}));
        QVERIFY(Boot());
        const QStringList after = Engine_->importPathList();
        QCOMPARE(after.mid(0, before.size()), before);
        QCOMPARE(after.size(), before.size() + 1);
        QVERIFY(after.last().endsWith("/a-one/qml"));
        // And the panel gets the Roost of roost.
        QVERIFY(Loader_->CreatePanel("a-one.p") != nullptr);
        QCOMPARE(Probe_.Last.value("shadow"), QString("undefined"));
    }

    void TheEarlierPluginKeepsAModuleUri() {
        const QJsonObject native{{"qmlImportPath", "qml"}};
        QVERIFY(AddPlugin(
            "a-first", {Panel("one", "p.qml")},
            Joined({{"p.qml", THING_PANEL}}, ThingModule("qml", "first")),
            "0.1.0", native));
        QVERIFY(AddPlugin(
            "b-second", {Panel("two", "p.qml")},
            Joined({{"p.qml", THING_PANEL}}, ThingModule("qml", "second")),
            "0.1.0", native));
        QVERIFY(Boot());
        QVERIFY(Loader_->Problems().isEmpty());
        QVERIFY(Loader_->CreatePanel("a-first.one") != nullptr);
        QVERIFY(Loader_->CreatePanel("b-second.two") != nullptr);
        // Two plugins that ship one URI share the engine's one module, and
        // the plugin that comes first in the load order owns it.
        QCOMPARE(Probe_.Count("label"), 2);
        QCOMPARE(Probe_.Last.value("label"), QString("first"));
    }

    void AChangedNativeReloadsThePlugin_data() {
        QTest::addColumn<bool>("declaredBefore");
        QTest::newRow("native added") << false;
        QTest::newRow("path changed") << true;
    }

    void AChangedNativeReloadsThePlugin() {
        QFETCH(bool, declaredBefore);
        // Both directories hold the module; only `native` tells where to look.
        const QMap<QString, QByteArray> files =
            Joined(Joined({{"p.qml", THING_PANEL}}, ThingModule("old", "old")),
                   ThingModule("new", "new"));
        QVERIFY(AddPlugin(
            "a-one", {Panel("p", "p.qml")}, files, "0.1.0",
            declaredBefore ? QJsonValue(QJsonObject{{"qmlImportPath", "old"}})
                           : QJsonValue(QJsonValue::Undefined)));
        if (!declaredBefore) {
            // The panel cannot import its module yet.
            ExpectWarnings(1);
        }
        QVERIFY(Boot());
        QCOMPARE(Loader_->PanelTypes().contains("a-one.p"), declaredBefore);
        QSignalSpy types(Loader_.get(), &PluginLoader::PanelTypesChanged);

        QVERIFY(Daemon_->Stop());
        QVERIFY(AddPlugin("a-one", {Panel("p", "p.qml")}, files, "0.1.0",
                          QJsonObject{{"qmlImportPath", "new"}}));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QTRY_COMPARE_WITH_TIMEOUT(types.count(), 1, TIMEOUT_MS * 2);

        QVERIFY(Loader_->Problems().isEmpty());
        QVERIFY(Loader_->CreatePanel("a-one.p") != nullptr);
        QCOMPARE(Probe_.Last.value("label"), QString("new"));
        // Only the directory that is declared now is on the import path.
        // (the engine keeps them with symlinks resolved)
        const QString directory =
            QFileInfo(Daemon_->PluginsPath() + "/a-one").canonicalFilePath() +
            "/";
        QVERIFY(Engine_->importPathList().contains(directory + "new"));
        QVERIFY(!Engine_->importPathList().contains(directory + "old"));
    }

    void APluginThatDropsNativeLeavesNoImportPath() {
        const auto files =
            Joined({{"p.qml", ItemPanel("p")}}, ThingModule("qml", "one"));
        QVERIFY(AddPlugin("a-one", {Panel("p", "p.qml")}, files, "0.1.0",
                          QJsonObject{{"qmlImportPath", "qml"}}));
        QVERIFY(Boot());
        // The directory stays on disk: the engine would drop one that is
        // gone, and the test would prove nothing.
        const QString path = QFileInfo(Daemon_->PluginsPath() + "/a-one/qml")
                                 .canonicalFilePath();
        QVERIFY(Engine_->importPathList().contains(path));
        QSignalSpy types(Loader_.get(), &PluginLoader::PanelTypesChanged);

        QVERIFY(Daemon_->Stop());
        QVERIFY(AddPlugin("a-one", {Panel("p", "p.qml")}, files));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QTRY_COMPARE_WITH_TIMEOUT(types.count(), 1, TIMEOUT_MS * 2);

        QVERIFY(QFileInfo(path).isDir());
        QVERIFY(!Engine_->importPathList().contains(path));
    }
    void APathThatLeftTheDiskDoesNotPinThePathOfAPlugin() {
        const auto files =
            Joined({{"p.qml", ItemPanel("p")}}, ThingModule("qml", "one"));
        QVERIFY(AddPlugin("a-one", {Panel("p", "p.qml")}, files, "0.1.0",
                          QJsonObject{{"qmlImportPath", "qml"}}));
        QVERIFY(Boot());
        const QString path = QFileInfo(Daemon_->PluginsPath() + "/a-one/qml")
                                 .canonicalFilePath();
        QVERIFY(Engine_->importPathList().contains(path));
        // Someone else's path, put before the plugin's, gone from the disk
        // by the next diff: the engine drops it on the next set.
        auto other = std::make_unique<QTemporaryDir>();
        QVERIFY(other->isValid());
        Engine_->addImportPath(other->path());
        QVERIFY(other->remove());
        other.reset();

        QSignalSpy types(Loader_.get(), &PluginLoader::PanelTypesChanged);
        QVERIFY(Daemon_->Stop());
        QVERIFY(AddPlugin("b-two", {Panel("q", "q.qml")},
                          {{"q.qml", ItemPanel("q")}}));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QTRY_COMPARE_WITH_TIMEOUT(types.count(), 1, TIMEOUT_MS * 2);
        QVERIFY(Engine_->importPathList().contains(path));

        QVERIFY(Daemon_->Stop());
        QVERIFY(AddPlugin("a-one", {Panel("p", "p.qml")}, files));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QTRY_COMPARE_WITH_TIMEOUT(types.count(), 2, TIMEOUT_MS * 2);
        QVERIFY(QFileInfo(path).isDir());
        QVERIFY(!Engine_->importPathList().contains(path));
    }

    void TwoPluginsThatNameOneDirectoryByDifferentPathsShareOneEntry() {
        const auto files =
            Joined({{"p.qml", ItemPanel("p")}}, ThingModule("qml", "one"));
        QVERIFY(AddPlugin("a-one", {Panel("p", "p.qml")}, files, "0.1.0",
                          QJsonObject{{"qmlImportPath", "qml"}}));
        QVERIFY(AddPlugin("b-two", {Panel("q", "q.qml")},
                          {{"q.qml", ItemPanel("q")}}, "0.1.0",
                          QJsonObject{{"qmlImportPath", "link"}}));
        // b-two reaches the directory of a-one through a symlink.
        const QString path = QFileInfo(Daemon_->PluginsPath() + "/a-one/qml")
                                 .canonicalFilePath();
        QVERIFY(QFile::link(path, Daemon_->PluginsPath() + "/b-two/link"));
        QVERIFY(Boot());
        QCOMPARE(Engine_->importPathList().count(path), 1);
        QSignalSpy types(Loader_.get(), &PluginLoader::PanelTypesChanged);

        // The plugin that stays still holds the entry.
        QVERIFY(Daemon_->Stop());
        QVERIFY(QDir(Daemon_->PluginsPath() + "/b-two").removeRecursively());
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QTRY_COMPARE_WITH_TIMEOUT(types.count(), 1, TIMEOUT_MS * 2);
        QCOMPARE(Engine_->importPathList().count(path), 1);

        QVERIFY(Daemon_->Stop());
        QVERIFY(AddPlugin("a-one", {Panel("p", "p.qml")}, files));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QTRY_COMPARE_WITH_TIMEOUT(types.count(), 2, TIMEOUT_MS * 2);
        QVERIFY(!Engine_->importPathList().contains(path));
    }
};

QTEST_MAIN(PluginLoaderTest)
#include "plugin_loader_test.moc"
