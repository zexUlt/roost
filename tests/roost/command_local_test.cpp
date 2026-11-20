// Commands that the window runs itself (CommandService::Register,
// Roost.commands.registerCommand), against the real roostd with the fixture
// backend. A counting proxy sits between the window and roostd and counts the
// commands.run requests that pass it: what the window runs itself never does,
// whatever the answer says. roostd handles one connection in order, so a
// request that goes to roostd after the case's own is counted by the time the
// answer to it is in.

#include <QDir>
#include <QFileInfo>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>

#include <memory>

#include "api/api.hpp"
#include "api/api_test_support.hpp"
#include "helpers/counting_proxy.hpp"
#include "helpers/roostd_process.hpp"
#include "qml_api/roost_singleton.hpp"
#include "roost/plugin_loader.hpp"

using namespace roost;
using namespace roost::api_test;
using roost::testing::CountingProxy;

// The static plugin of module Roost.Api, linked into this executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)

namespace {

constexpr int INVALID_PARAMS = static_cast<int>(rpc::EErrorCode::InvalidParams);
constexpr int INTERNAL_ERROR = static_cast<int>(rpc::EErrorCode::InternalError);

// A panel that gives commands handlers when it is created.
const QByteArray PANEL = R"(
import QtQuick
import Roost.Api
Item {
    property bool hello: false
    Component.onCompleted: {
        hello = Roost.commands.registerCommand("panel.hello", args => ({from: "panel", args: args}))
        Roost.commands.registerCommand("panel.nothing", () => {})
        Roost.commands.registerCommand("panel.throws", () => { throw new Error("boom") })
    }
    function again() { return Roost.commands.registerCommand("panel.hello", () => 1) }
    function notAFunction() {
        try {
            Roost.commands.registerCommand("panel.bad", 42)
        } catch (e) {
            return e.name
        }
        return ""
    }
}
)";

// The shell's own QML: not a panel, so what it registers is the window's.
const QByteArray SHELL = R"(
import QtQml
import Roost.Api
QtObject {
    function add() {
        return Roost.commands.registerCommand("shell.hello", () => "from the shell")
    }
}
)";

}  // namespace

class CommandLocalTest : public QObject {
    Q_OBJECT

private:
    bool Boot() {
        if (!Daemon_->IsValid() ||
            !Daemon_->AddFixturePlugin("fixture", FIXTURE_BACKEND_PATH) ||
            !Daemon_->AddQmlPlugin("local-one",
                                   {QJsonObject{{"id", "p"}, {"qml", "p.qml"}}},
                                   {{"p.qml", PANEL}}) ||
            !Daemon_->Start(ROOSTD_PATH)) {
            return false;
        }
        Proxy_ = std::make_unique<CountingProxy>(
            QFileInfo(Daemon_->SocketPath()).dir().filePath("proxy.sock"),
            Daemon_->SocketPath());
        if (!Proxy_->Listen()) {
            return false;
        }
        Api_ = std::make_unique<api::Api>();
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        Engine_ = std::make_unique<QQmlEngine>();
        Singleton_ =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        if (Singleton_ == nullptr) {
            return false;
        }
        Singleton_->setApi(*Api_);
        Api_->Connect(Proxy_->Path());
        if (connected.isEmpty() && !connected.wait(TIMEOUT_MS)) {
            return false;
        }
        if (!QTest::qWaitFor([&] { return Api_->Plugins().IsLoaded(); },
                             TIMEOUT_MS)) {
            return false;
        }
        Loader_ = std::make_unique<PluginLoader>(*Engine_, Api_->Plugins(),
                                                 *Singleton_);
        return true;
    }

    rpc::Result Run(const QString& command,
                    const QJsonValue& args = QJsonValue::Undefined) {
        const QFuture<rpc::Result> future = Api_->Commands().Run(command, args);
        if (!WaitFor(future)) {
            return std::unexpected(rpc::Error{.Message = "timed out"});
        }
        return future.result();
    }

    // The requests of roostd that the proxy has seen, after a command that
    // goes to roostd (`fixture.echo`) is answered: roostd answers in order,
    // so everything sent before it is counted.
    int RequestsSeenByRoostd() {
        const rpc::Result sentinel = Run("fixture.echo", "sentinel");
        if (!sentinel || *sentinel != QJsonValue("sentinel")) {
            return -1;
        }
        // The sentinel is one of them.
        return Proxy_->CommandRequests() - 1;
    }

    std::unique_ptr<roost::testing::RoostdProcess> Daemon_;
    std::unique_ptr<CountingProxy> Proxy_;
    std::unique_ptr<api::Api> Api_;
    std::unique_ptr<QQmlEngine> Engine_;
    RoostSingleton* Singleton_ = nullptr;
    std::unique_ptr<PluginLoader> Loader_;

private slots:
    void init() { Daemon_ = std::make_unique<roost::testing::RoostdProcess>(); }

    void cleanup() {
        Loader_.reset();
        Engine_.reset();
        Singleton_ = nullptr;
        Api_.reset();
        Proxy_.reset();
        Daemon_.reset();
    }

    void ALocalCommandRunsWithoutRoostd() {
        QVERIFY(Boot());
        QObject owner;
        QVERIFY(Api_->Commands().Register(
            "local.hello", &owner, [](const QJsonValue& args) {
                return rpc::Result(QJsonObject{{"got", args}});
            }));
        QVERIFY(Api_->Commands().HasLocal("local.hello"));

        const rpc::Result answer = Run("local.hello", QJsonArray{1, "two"});
        QVERIFY2(answer, qPrintable(Why(answer)));
        QCOMPARE(*answer,
                 QJsonValue(QJsonObject{{"got", QJsonArray{1, "two"}}}));
        // Also by the name through the dispatcher, as Roost.call does.
        const QFuture<rpc::Result> called = Api_->Dispatch().Call(
            "commands.run", QJsonObject{{"command", "local.hello"}}, this);
        QVERIFY(WaitFor(called));
        QCOMPARE(*called.result(), QJsonValue(QJsonObject()));
        QCOMPARE(RequestsSeenByRoostd(), 0);
    }

    // A command that a plugin declares is the window's if the window has a
    // handler for it, and roostd's again when the handler goes.
    void ALocalHandlerComesBeforeTheBackendOfTheCommand() {
        QVERIFY(Boot());
        auto owner = std::make_unique<QObject>();
        QVERIFY(Api_->Commands().Register(
            "fixture.echo", owner.get(),
            [](const QJsonValue&) { return rpc::Result("local"); }));
        QCOMPARE(*Run("fixture.echo", "x"), QJsonValue("local"));
        QCOMPARE(Proxy_->CommandRequests(), 0);

        owner.reset();
        QVERIFY(!Api_->Commands().HasLocal("fixture.echo"));
        QCOMPARE(*Run("fixture.echo", "x"), QJsonValue("x"));
        QCOMPARE(Proxy_->CommandRequests(), 1);
    }

    void AnUnknownIdGoesToRoostd() {
        QVERIFY(Boot());
        QObject owner;
        QVERIFY(Api_->Commands().Register(
            "local.hello", &owner,
            [](const QJsonValue&) { return rpc::Result(QJsonValue::Null); }));
        const rpc::Result answer = Run("no.such");
        QVERIFY(!answer);
        QCOMPARE(answer.error().Code, INVALID_PARAMS);
        QVERIFY2(answer.error().Message.contains("no.such"),
                 qPrintable(answer.error().Message));
        QCOMPARE(Proxy_->CommandRequests(), 1);
        // A declared command of a plugin goes the same way.
        QCOMPARE(*Run("fixture.echo", 5), QJsonValue(5));
        QCOMPARE(Proxy_->CommandRequests(), 2);
    }

    void AHandlerIsNotReplaced() {
        QVERIFY(Boot());
        QObject first;
        auto second = std::make_unique<QObject>();
        QVERIFY(Api_->Commands().Register(
            "local.hello", &first,
            [](const QJsonValue&) { return rpc::Result("first"); }));
        QVERIFY(!Api_->Commands().Register(
            "local.hello", second.get(),
            [](const QJsonValue&) { return rpc::Result("second"); }));
        // The end of the owner that was refused takes nothing away.
        second.reset();
        QVERIFY(Api_->Commands().HasLocal("local.hello"));
        QCOMPARE(*Run("local.hello"), QJsonValue("first"));
    }

    void ACommandRegisteredByAPanelGoesWithThePanel() {
        QVERIFY(Boot());
        QQuickItem* panel = Loader_->CreatePanel("local-one.p");
        if (panel == nullptr) {
            QFAIL("the panel was not created");
        }
        QVERIFY(panel->property("hello").toBool());

        const rpc::Result hello = Run("panel.hello", QJsonObject{{"a", 1}});
        QVERIFY2(hello, qPrintable(Why(hello)));
        QCOMPARE(*hello,
                 QJsonValue(QJsonObject{{"from", "panel"},
                                        {"args", QJsonObject{{"a", 1}}}}));
        // A handler that returns nothing answers null; one that throws, an
        // error that says why.
        QCOMPARE(*Run("panel.nothing"), QJsonValue(QJsonValue::Null));
        const rpc::Result thrown = Run("panel.throws");
        QVERIFY(!thrown);
        QCOMPARE(thrown.error().Code, INTERNAL_ERROR);
        QVERIFY2(thrown.error().Message.contains("boom"),
                 qPrintable(thrown.error().Message));
        // A second handler for the id is refused, and so is one that is not a
        // function.
        QTest::ignoreMessage(
            QtWarningMsg, QRegularExpression(
                              "^roost: the command panel.hello has a handler"));
        QVariant again;
        QVERIFY(QMetaObject::invokeMethod(panel, "again",
                                          Q_RETURN_ARG(QVariant, again)));
        QVERIFY(!again.toBool());
        QVariant name;
        QVERIFY(QMetaObject::invokeMethod(panel, "notAFunction",
                                          Q_RETURN_ARG(QVariant, name)));
        QCOMPARE(name.toString(), QString("TypeError"));
        QVERIFY(!Api_->Commands().HasLocal("panel.bad"));
        QCOMPARE(Proxy_->CommandRequests(), 0);

        // The panel is torn down: its commands are roostd's again, which does
        // not know them.
        Loader_->DestroyPanel(panel);
        QVERIFY(!Api_->Commands().HasLocal("panel.hello"));
        QVERIFY(!Api_->Commands().HasLocal("panel.nothing"));
        QVERIFY(!Api_->Commands().HasLocal("panel.throws"));
        const rpc::Result gone = Run("panel.hello");
        QVERIFY(!gone);
        QCOMPARE(gone.error().Code, INVALID_PARAMS);
        QCOMPARE(Proxy_->CommandRequests(), 1);

        // And a new panel has them again.
        QQuickItem* fresh = Loader_->CreatePanel("local-one.p");
        QVERIFY(fresh != nullptr);
        QVERIFY(fresh->property("hello").toBool());
        QCOMPARE((*Run("panel.hello")).toObject().value("from"),
                 QJsonValue("panel"));
    }

    // The shell is not a panel: what its QML registers lives as long as the
    // window, whatever happens to the panels.
    void ACommandRegisteredByTheShellStays() {
        QVERIFY(Boot());
        QQmlComponent component(Engine_.get());
        component.setData(SHELL, QUrl());
        std::unique_ptr<QObject> shell(component.create());
        QVERIFY2(shell != nullptr, qPrintable(component.errorString()));
        QVariant added;
        QVERIFY(QMetaObject::invokeMethod(shell.get(), "add",
                                          Q_RETURN_ARG(QVariant, added)));
        QVERIFY(added.toBool());

        QQuickItem* panel = Loader_->CreatePanel("local-one.p");
        QVERIFY(panel != nullptr);
        Loader_->DestroyPanel(panel);
        QCOMPARE(*Run("shell.hello"), QJsonValue("from the shell"));
        QCOMPARE(Proxy_->CommandRequests(), 0);
    }
};

QTEST_MAIN(CommandLocalTest)
#include "command_local_test.moc"
