// The window's shell: Main.qml of module Roost.Shell with its two panel slots,
// the PluginLoader and the Shell controller over the real roostd (plugins with
// fixture QML panels), and a server that refuses hello. What the user sees is
// read from the QML objects: the properties of the slots and the root, the
// placeholder, the banner and the strip.

#include <QDir>
#include <QFile>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "api/api.hpp"
#include "api/api_test_support.hpp"
#include "core/version.hpp"
#include "helpers/refusing_server.hpp"
#include "helpers/roostd_process.hpp"
#include "qml_api/roost_singleton.hpp"
#include "roost/plugin_loader.hpp"
#include "roost/shell.hpp"

using namespace roost;
using namespace roost::api_test;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

const QString LIST = "sessions.list";
const QString TERMINAL = "terminal.view";

// A list panel: `tag` tells the version of the file, `selected` is its output.
QByteArray ListPanel(const QString& tag = "v1") {
    return QStringLiteral(R"(
import QtQuick
Item {
    property string tag: "%1"
    property var selected: null
}
)")
        .arg(tag)
        .toUtf8();
}

// A list panel whose output is not initialised: it reads as undefined.
const QByteArray UNSET_LIST_PANEL = R"(
import QtQuick
Item {
    property var selected
}
)";

const QByteArray TERMINAL_PANEL = R"(
import QtQuick
Item {
    property var session: null
}
)";

// Two outputs of the same type.
const QByteArray TWO_OUTPUTS_PANEL = R"(
import QtQuick
Item {
    property var first: null
    property var second: null
}
)";

// A panel that has none of the properties its manifest declares.
const QByteArray BARE_PANEL = "import QtQuick\nItem {}\n";

const QByteArray BROKEN_PANEL = "import QtQuick\nItem {{\n";

QJsonObject Panel(const QString& id, const QString& qml,
                  const QJsonObject& inputs = {},
                  const QJsonObject& outputs = {}) {
    return {{"id", id}, {"qml", qml}, {"inputs", inputs}, {"outputs", outputs}};
}

QJsonObject ListEntry(const QJsonObject& outputs = {
                          {"selected", "SessionRef"}}) {
    return Panel("list", "list.qml", {}, outputs);
}

QJsonObject TerminalEntry(const QJsonObject& inputs = {
                              {"session", "SessionRef"}}) {
    return Panel("view", "terminal.qml", inputs);
}

QVariantMap Ref(const QString& agent, const QString& id) {
    return {{"agent", agent}, {"id", id}};
}

// A var property as C++ reads it, whichever way it was written.
QVariantMap AsMap(const QVariant& value) {
    if (value.metaType() == QMetaType::fromType<QJSValue>()) {
        return value.value<QJSValue>().toVariant().toMap();
    }
    return value.toMap();
}

}  // namespace

class ShellTest : public QObject {
    Q_OBJECT

private:
    // Plugins first (Daemon_->AddQmlPlugin), then this. Starts roostd unless
    // `socketPath` is given: then the window connects there instead.
    bool Boot(const QString& socketPath = {}) {
        if (!Daemon_->IsValid()) {
            return false;
        }
        if (socketPath.isEmpty() && !Daemon_->Start(ROOSTD_PATH)) {
            return false;
        }
        Api_ = std::make_unique<api::Api>();
        Engine_ = std::make_unique<QQmlApplicationEngine>();
        Singleton_ =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        if (Singleton_ == nullptr) {
            return false;
        }
        Singleton_->setApi(*Api_);
        const QString socket =
            socketPath.isEmpty() ? Daemon_->SocketPath() : socketPath;
        Socket_ = socket;
        Api_->Connect(socket);
        Engine_->loadFromModule("Roost.Shell", "Main");
        if (Engine_->rootObjects().isEmpty()) {
            return false;
        }
        Loader_ = std::make_unique<PluginLoader>(*Engine_, Api_->Plugins(),
                                                 *Singleton_);
        Shell_ = std::make_unique<Shell>(*Engine_->rootObjects().first(),
                                         *Loader_, *Api_, socket);
        return true;
    }

    // Boots with the two panels of the plugins given.
    bool BootPlugins(const QJsonObject& list, const QByteArray& listQml,
                     const QJsonObject& terminal,
                     const QByteArray& terminalQml) {
        return Daemon_->AddQmlPlugin("sessions", {list},
                                     {{"list.qml", listQml}}) &&
               Daemon_->AddQmlPlugin("terminal", {terminal},
                                     {{"terminal.qml", terminalQml}}) &&
               Boot();
    }

    QObject* Root() { return Engine_->rootObjects().first(); }

    QQuickItem* Slot(const QString& type) {
        for (auto* item : Root()->findChildren<QQuickItem*>("panelSlot")) {
            if (item->property("panelType").toString() == type) {
                return item;
            }
        }
        return nullptr;
    }

    QQuickItem* PanelOf(const QString& type) {
        QQuickItem* slot = Slot(type);
        return slot == nullptr ? nullptr
                               : slot->property("panel").value<QQuickItem*>();
    }

    QString ErrorOf(const QString& type) {
        return Slot(type)->property("error").toString();
    }

    QQuickItem* Placeholder(const QString& type) {
        return Slot(type)->findChild<QQuickItem*>("placeholderText");
    }

    QString Connection() { return Root()->property("connection").toString(); }

    QStringList Problems() {
        return Root()->property("problems").toStringList();
    }

    bool HasProblem(const QString& part) {
        return std::ranges::any_of(
            Problems(), [&](const QString& p) { return p.contains(part); });
    }

    bool BothPanelsShown() {
        return PanelOf(LIST) != nullptr && PanelOf(TERMINAL) != nullptr;
    }

    // Whether roostd has loaded the plugins and the slots have settled.
    bool Settled() {
        return Connection() == "connected" && Api_->Plugins().IsLoaded() &&
               ErrorOf(LIST) != "waiting for roostd" &&
               ErrorOf(TERMINAL) != "waiting for roostd";
    }

    std::unique_ptr<roost::testing::RoostdProcess> Daemon_;
    std::unique_ptr<api::Api> Api_;
    std::unique_ptr<QQmlApplicationEngine> Engine_;
    RoostSingleton* Singleton_ = nullptr;
    std::unique_ptr<PluginLoader> Loader_;
    std::unique_ptr<Shell> Shell_;
    QString Socket_;

private slots:
    void init() { Daemon_ = std::make_unique<roost::testing::RoostdProcess>(); }

    void cleanup() {
        // The shell before the loader, the loader before the engine.
        Shell_.reset();
        Loader_.reset();
        Engine_.reset();
        Singleton_ = nullptr;
        Api_.reset();
        Daemon_.reset();
    }

    void WithoutRoostdTheSlotsWaitAndTheWindowIsDisconnected() {
        QVERIFY(Boot(Daemon_->SocketPath()));
        QCOMPARE(Connection(), QString("connecting"));
        QTRY_COMPARE_WITH_TIMEOUT(Connection(), QString("disconnected"),
                                  TIMEOUT_MS);
        QVERIFY(!Root()->property("connectionMessage").toString().isEmpty());
        for (const QString& type : {LIST, TERMINAL}) {
            QVERIFY(Slot(type) != nullptr);
            QVERIFY(PanelOf(type) == nullptr);
            QCOMPARE(ErrorOf(type), QString("waiting for roostd"));
            QVERIFY(Placeholder(type)->isVisible());
            QCOMPARE(Placeholder(type)->property("text").toString(),
                     QString("waiting for roostd"));
        }
        auto* banner = Root()->findChild<QQuickItem*>("connectionBanner");
        QVERIFY(banner != nullptr);
        QVERIFY(banner->isVisible());
        QVERIFY(Problems().isEmpty());
    }

    void TheListSelectionReachesTheTerminalAndSurvivesAReloadOfTheList() {
        QTest::failOnWarning();
        QVERIFY(BootPlugins(ListEntry(), ListPanel(), TerminalEntry(),
                            TERMINAL_PANEL));
        QTRY_VERIFY_WITH_TIMEOUT(BothPanelsShown(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Connection(), QString("connected"),
                                  TIMEOUT_MS);
        QVERIFY(Problems().isEmpty());
        auto* banner = Root()->findChild<QQuickItem*>("connectionBanner");
        QVERIFY(!banner->isVisible());

        QQuickItem* list = PanelOf(LIST);
        QQuickItem* terminal = PanelOf(TERMINAL);
        QCOMPARE(list->parentItem(), Slot(LIST));
        QCOMPARE(terminal->parentItem(), Slot(TERMINAL));
        QVERIFY(!Placeholder(LIST)->isVisible());
        QCOMPARE(Slot(LIST)->property("linkOutput").toString(),
                 QString("selected"));
        QCOMPARE(Slot(TERMINAL)->property("linkInput").toString(),
                 QString("session"));

        QVERIFY(list->setProperty("selected", Ref("a", "s1")));
        QTRY_COMPARE_WITH_TIMEOUT(AsMap(terminal->property("session")),
                                  Ref("a", "s1"), TIMEOUT_MS);
        QVERIFY(list->setProperty("selected", Ref("a", "s2")));
        QTRY_COMPARE_WITH_TIMEOUT(AsMap(terminal->property("session")),
                                  Ref("a", "s2"), TIMEOUT_MS);

        // The list plugin gets a new version and roostd restarts: the window
        // reconnects, the loader reloads the plugin and the slot gets the new
        // panel, linked again.
        QPointer<QQuickItem> oldList = list;
        QVERIFY(Daemon_->Stop());
        QVERIFY(Daemon_->AddQmlPlugin("sessions", {ListEntry()},
                                      {{"list.qml", ListPanel("v2")}},
                                      "0.2.0"));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QTRY_VERIFY_WITH_TIMEOUT(
            PanelOf(LIST) != nullptr &&
                PanelOf(LIST)->property("tag").toString() == "v2",
            TIMEOUT_MS * 2);
        QVERIFY(oldList.isNull());
        // The terminal panel was not touched.
        QCOMPARE(PanelOf(TERMINAL), terminal);
        QTRY_COMPARE_WITH_TIMEOUT(Connection(), QString("connected"),
                                  TIMEOUT_MS);
        QVERIFY(!banner->isVisible());
        QVERIFY(PanelOf(LIST)->setProperty("selected", Ref("b", "s9")));
        QTRY_COMPARE_WITH_TIMEOUT(AsMap(terminal->property("session")),
                                  Ref("b", "s9"), TIMEOUT_MS);
    }

    void ABrokenTerminalShowsItsErrorAndRoostdsProblemsAreInTheStrip() {
        QTest::failOnWarning();
        // The loader's own report of the broken file is expected.
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression("plugin terminal, panel terminal.view: "));
        // A plugin whose manifest is not JSON: roostd reports it.
        QVERIFY(QDir().mkpath(Daemon_->PluginsPath() + "/broken-manifest"));
        QFile manifest(Daemon_->PluginsPath() + "/broken-manifest/plugin.json");
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        QVERIFY(manifest.write("{ not json") > 0);
        manifest.close();

        QVERIFY(BootPlugins(ListEntry(), ListPanel(), TerminalEntry(),
                            BROKEN_PANEL));
        QTRY_VERIFY_WITH_TIMEOUT(Settled() && PanelOf(LIST) != nullptr,
                                 TIMEOUT_MS);
        QVERIFY(PanelOf(TERMINAL) == nullptr);
        const QString error = ErrorOf(TERMINAL);
        QVERIFY2(error.contains("terminal.qml"), qPrintable(error));
        QVERIFY(Placeholder(TERMINAL)->isVisible());
        QCOMPARE(Placeholder(TERMINAL)->property("text").toString(), error);
        QVERIFY(HasProblem("plugin terminal, panel terminal.view: "));
        QVERIFY(HasProblem("broken-manifest"));
        QVERIFY(!HasProblem("plugin sessions"));
        auto* strip = Root()->findChild<QQuickItem*>("problemsStrip");
        QVERIFY(strip->isVisible());
        // Not linked: the terminal has no panel, and the selection warns
        // about nothing (failOnWarning).
        QVERIFY(PanelOf(LIST)->setProperty("selected", Ref("a", "s1")));
        QTest::qWait(100);
        QVERIFY(PanelOf(TERMINAL) == nullptr);
        QVERIFY(Slot(TERMINAL)->property("panel").value<QQuickItem*>() ==
                nullptr);
    }

    void ATypeNobodyOffersIsNamedInThePlaceholder() {
        QVERIFY(Daemon_->AddQmlPlugin("sessions", {ListEntry()},
                                      {{"list.qml", ListPanel()}}));
        QVERIFY(Boot());
        QTRY_VERIFY_WITH_TIMEOUT(Settled() && PanelOf(LIST) != nullptr,
                                 TIMEOUT_MS);
        QCOMPARE(ErrorOf(TERMINAL), QString("no panel terminal.view"));
        QCOMPARE(Placeholder(TERMINAL)->property("text").toString(),
                 QString("no panel terminal.view"));
    }

    void ADeclaredInputTheRootLacksIsAProblemAndTheLinkIsOff() {
        QVERIFY(BootPlugins(ListEntry(), ListPanel(),
                            TerminalEntry({{"session", "SessionRef"}}),
                            BARE_PANEL));
        QTRY_VERIFY_WITH_TIMEOUT(BothPanelsShown(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            HasProblem("plugin terminal, panel terminal.view: declared "
                       "input \"session\""),
            TIMEOUT_MS);
        QVERIFY(Slot(TERMINAL)->property("linkInput").toString().isEmpty());
        QVERIFY(!Placeholder(TERMINAL)->isVisible());
        // Nothing to write to: the selection goes nowhere, nothing breaks.
        QVERIFY(PanelOf(LIST)->setProperty("selected", Ref("a", "s1")));
        QTest::qWait(100);
        QVERIFY(PanelOf(TERMINAL)->property("session").isNull() ||
                !PanelOf(TERMINAL)->property("session").isValid());
    }

    void AMissingInputOfAnotherTypeLeavesTheLinkOn() {
        QVERIFY(BootPlugins(
            ListEntry(), ListPanel(),
            TerminalEntry({{"session", "SessionRef"}, {"volume", "Number"}}),
            TERMINAL_PANEL));
        QTRY_VERIFY_WITH_TIMEOUT(BothPanelsShown(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem("declared input \"volume\""),
                                 TIMEOUT_MS);
        QCOMPARE(Slot(TERMINAL)->property("linkInput").toString(),
                 QString("session"));
        QVERIFY(PanelOf(LIST)->setProperty("selected", Ref("a", "s1")));
        QTRY_COMPARE_WITH_TIMEOUT(AsMap(PanelOf(TERMINAL)->property("session")),
                                  Ref("a", "s1"), TIMEOUT_MS);
    }

    void TwoSessionRefOutputsSwitchTheLinkOffWithAProblem() {
        QVERIFY(BootPlugins(
            ListEntry({{"first", "SessionRef"}, {"second", "SessionRef"}}),
            TWO_OUTPUTS_PANEL, TerminalEntry(), TERMINAL_PANEL));
        QTRY_VERIFY_WITH_TIMEOUT(BothPanelsShown(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem("sessions.list declares 2 output"),
                                 TIMEOUT_MS);
        QVERIFY(Slot(LIST)->property("linkOutput").toString().isEmpty());
        // The terminal keeps what it has: nothing is written to its input.
        QVERIFY(PanelOf(TERMINAL)->setProperty("session", Ref("x", "kept")));
        QVERIFY(PanelOf(LIST)->setProperty("first", Ref("a", "s1")));
        QTest::qWait(100);
        QCOMPARE(AsMap(PanelOf(TERMINAL)->property("session")),
                 Ref("x", "kept"));
    }

    void AListWithoutASessionRefOutputIsAProblemToo() {
        QVERIFY(BootPlugins(ListEntry({}), ListPanel(), TerminalEntry(),
                            TERMINAL_PANEL));
        QTRY_VERIFY_WITH_TIMEOUT(BothPanelsShown(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem("sessions.list declares no output"),
                                 TIMEOUT_MS);
        QVERIFY(Slot(LIST)->property("linkOutput").toString().isEmpty());
    }

    void WhenRoostdStopsThePanelsStayAndTheBannerAppears() {
        QVERIFY(BootPlugins(ListEntry(), ListPanel(), TerminalEntry(),
                            TERMINAL_PANEL));
        QTRY_VERIFY_WITH_TIMEOUT(BothPanelsShown(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Connection(), QString("connected"),
                                  TIMEOUT_MS);
        QQuickItem* list = PanelOf(LIST);
        QQuickItem* terminal = PanelOf(TERMINAL);
        auto* banner = Root()->findChild<QQuickItem*>("connectionBanner");
        QVERIFY(!banner->isVisible());

        QVERIFY(Daemon_->Stop());
        QTRY_COMPARE_WITH_TIMEOUT(Connection(), QString("disconnected"),
                                  TIMEOUT_MS);
        QVERIFY(banner->isVisible());
        QCOMPARE(PanelOf(LIST), list);
        QCOMPARE(PanelOf(TERMINAL), terminal);
        QVERIFY(!Placeholder(LIST)->isVisible());
        // The link still works on what is shown.
        QVERIFY(list->setProperty("selected", Ref("a", "s3")));
        QTRY_COMPARE_WITH_TIMEOUT(AsMap(terminal->property("session")),
                                  Ref("a", "s3"), TIMEOUT_MS);
    }

    void ARefusedHelloShowsTheVersionsAndRetryAsksAgain() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        roost::testing::RefusingServer server;
        QVERIFY(server.Listen(dir.filePath("old.sock")));
        QVERIFY(Boot(dir.filePath("old.sock")));
        QTRY_COMPARE_WITH_TIMEOUT(Connection(), QString("refused"), TIMEOUT_MS);
        const QString message =
            Root()->property("connectionMessage").toString();
        QVERIFY2(
            message.contains("protocol 0") &&
                message.contains(roost::testing::RefusingServer::GHOSTTY_VT) &&
                message.contains(QString::fromStdString(GhosttyVtVersion())) &&
                message.contains("Restart roostd"),
            qPrintable(message));
        auto* retry = Root()->findChild<QQuickItem*>("retryButton");
        QVERIFY(retry != nullptr);
        QVERIFY(retry->isVisible());
        QCOMPARE(PanelOf(LIST), nullptr);
        for (const QString& type : {LIST, TERMINAL}) {
            QCOMPARE(ErrorOf(type), QString("roostd refused the connection"));
            QCOMPARE(Placeholder(type)->property("text").toString(),
                     QString("roostd refused the connection"));
        }
        // The Disconnected that follows a refusal does not hide it.
        QTest::qWait(300);
        QCOMPARE(Connection(), QString("refused"));
        QCOMPARE(server.Hellos(), 1);

        QVERIFY(QMetaObject::invokeMethod(Root(), "retryRequested"));
        QTRY_COMPARE_WITH_TIMEOUT(server.Hellos(), 2, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Connection(), QString("refused"), TIMEOUT_MS);
        // The button itself.
        QVERIFY(QMetaObject::invokeMethod(retry, "clicked"));
        QTRY_COMPARE_WITH_TIMEOUT(server.Hellos(), 3, TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Connection(), QString("refused"), TIMEOUT_MS);
    }

    void AnUninitialisedListOutputGivesTheTerminalNullNotUndefined() {
        QVERIFY(BootPlugins(ListEntry(), UNSET_LIST_PANEL, TerminalEntry(),
                            TERMINAL_PANEL));
        QTRY_VERIFY_WITH_TIMEOUT(BothPanelsShown(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            Slot(TERMINAL)->property("linkInput").toString(),
            QString("session"), TIMEOUT_MS);
        QQuickItem* terminal = PanelOf(TERMINAL);
        // The list's own value is undefined.
        QVERIFY(!PanelOf(LIST)->property("selected").isValid() ||
                PanelOf(LIST)->property("selected").isNull());
        QQmlExpression strictNull(Engine_->rootContext(), terminal,
                                  "session === null");
        QTRY_VERIFY_WITH_TIMEOUT(strictNull.evaluate().toBool(), TIMEOUT_MS);
        QVERIFY(!strictNull.hasError());
        // Not the C++ undefined either.
        const QVariant session = terminal->property("session");
        QVERIFY(session.isValid());
    }

    void TheSlotsPanelIsAlreadyNullWhenThePanelIsAboutToBeDestroyed() {
        QVERIFY(BootPlugins(ListEntry(), ListPanel(), TerminalEntry(),
                            TERMINAL_PANEL));
        QTRY_VERIFY_WITH_TIMEOUT(BothPanelsShown(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Connection(), QString("connected"),
                                  TIMEOUT_MS);
        // Connected after the Shell: its slot ran first.
        int delivered = 0;
        bool slotPanelWasNull = false;
        // The context dies with this function: the teardown in cleanup emits
        // the signal again, after these locals are gone.
        QObject scope;
        connect(
            Loader_.get(), &PluginLoader::PanelAboutToBeDestroyed, &scope,
            [&](QQuickItem* panel) {
                if (panel->property("tag").isValid()) {
                    ++delivered;
                    slotPanelWasNull =
                        Slot(LIST)->property("panel").value<QQuickItem*>() ==
                        nullptr;
                }
            },
            Qt::DirectConnection);
        QVERIFY(Daemon_->Stop());
        QVERIFY(Daemon_->AddQmlPlugin("sessions", {ListEntry()},
                                      {{"list.qml", ListPanel("v2")}},
                                      "0.2.0"));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QTRY_VERIFY_WITH_TIMEOUT(
            PanelOf(LIST) != nullptr &&
                PanelOf(LIST)->property("tag").toString() == "v2",
            TIMEOUT_MS * 2);
        QCOMPARE(delivered, 1);
        QVERIFY(slotPanelWasNull);
    }

    void ARemovedPluginsSlotGetsThePlaceholderAndItsLinkProblemGoes() {
        // Two outputs: a link problem in the strip while the plugin lives.
        QVERIFY(BootPlugins(
            ListEntry({{"first", "SessionRef"}, {"second", "SessionRef"}}),
            TWO_OUTPUTS_PANEL, TerminalEntry(), TERMINAL_PANEL));
        QTRY_VERIFY_WITH_TIMEOUT(BothPanelsShown(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(HasProblem("sessions.list declares 2 output"),
                                 TIMEOUT_MS);

        QVERIFY(Daemon_->Stop());
        QVERIFY(QDir(Daemon_->PluginsPath() + "/sessions").removeRecursively());
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QTRY_VERIFY_WITH_TIMEOUT(PanelOf(LIST) == nullptr && Settled(),
                                 TIMEOUT_MS * 2);
        QCOMPARE(ErrorOf(LIST), QString("no panel sessions.list"));
        QVERIFY(Placeholder(LIST)->isVisible());
        QCOMPARE(Placeholder(LIST)->property("text").toString(),
                 QString("no panel sessions.list"));
        QVERIFY(Slot(LIST)->property("linkOutput").toString().isEmpty());
        QVERIFY(Slot(LIST)->property("linkInput").toString().isEmpty());
        QVERIFY(!HasProblem("sessions.list declares"));
        // The terminal stays, and its link is off for want of the list.
        QVERIFY(PanelOf(TERMINAL) != nullptr);
    }

    void TheBannerTellsConnectingAndDisconnectedAndRetryIsOnlyForRefused() {
        QVERIFY(Boot(Daemon_->SocketPath()));
        auto* banner = Root()->findChild<QQuickItem*>("connectionBanner");
        auto* text = Root()->findChild<QQuickItem*>("connectionText");
        auto* retry = Root()->findChild<QQuickItem*>("retryButton");
        QVERIFY(banner != nullptr && text != nullptr && retry != nullptr);
        QCOMPARE(Connection(), QString("connecting"));
        QVERIFY(banner->isVisible());
        QCOMPARE(text->property("text").toString(),
                 QString("Connecting to roostd..."));
        QVERIFY(!retry->isVisible());

        QTRY_COMPARE_WITH_TIMEOUT(Connection(), QString("disconnected"),
                                  TIMEOUT_MS);
        QVERIFY(banner->isVisible());
        const QString disconnected = text->property("text").toString();
        QVERIFY2(
            disconnected.startsWith("Disconnected from roostd, reconnecting."),
            qPrintable(disconnected));
        QVERIFY(disconnected.size() >
                QString("Disconnected from roostd, reconnecting.").size());
        QVERIFY(!retry->isVisible());
    }

    void ARefusalWithoutDataHasNoVersionsAndKeepsTheRestartHint() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        roost::testing::RefusingServer server;
        server.SetOmitData(true);
        QVERIFY(server.Listen(dir.filePath("old.sock")));
        QVERIFY(Boot(dir.filePath("old.sock")));
        QTRY_COMPARE_WITH_TIMEOUT(Connection(), QString("refused"), TIMEOUT_MS);
        const QString message =
            Root()->property("connectionMessage").toString();
        // The versions part is the one Shell appends: "(roostd: protocol ...".
        QVERIFY2(!message.contains("this window"), qPrintable(message));
        QVERIFY2(!message.contains("(roostd: protocol"), qPrintable(message));
        QVERIFY2(message.endsWith(". Restart roostd, then press Retry."),
                 qPrintable(message));
        for (const QString& type : {LIST, TERMINAL}) {
            QCOMPARE(ErrorOf(type), QString("roostd refused the connection"));
        }
    }

    // A window without the slots of the two panel types cannot become ready by
    // waiting: CheckReadiness fails and names both.
    void AWindowWithoutPanelSlotsFailsTheReadinessCheck() {
        QVERIFY(Daemon_->IsValid());
        Api_ = std::make_unique<api::Api>();
        Engine_ = std::make_unique<QQmlApplicationEngine>();
        Singleton_ =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        QVERIFY(Singleton_ != nullptr);
        Singleton_->setApi(*Api_);
        Loader_ = std::make_unique<PluginLoader>(*Engine_, Api_->Plugins(),
                                                 *Singleton_);
        QObject bareRoot;
        Shell_ = std::make_unique<Shell>(bareRoot, *Loader_, *Api_,
                                         Daemon_->SocketPath());
        const Shell::Readiness readiness = Shell_->CheckReadiness();
        QVERIFY(readiness.Failed);
        QVERIFY2(readiness.Reason.contains("the window has no panel "
                                           "sessions.list") &&
                     readiness.Reason.contains("the window has no panel "
                                               "terminal.view"),
                 qPrintable(readiness.Reason));
    }
};

QTEST_MAIN(ShellTest)
#include "shell_test.moc"
