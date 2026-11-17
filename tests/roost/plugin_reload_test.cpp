// PluginWatcher, the developer mode of the window (`roost --dev`): the QML of
// a plugin is reloaded when its files change on disk. Against the real roostd
// (plugins with QML panels), a real engine, the PluginLoader and, in one
// test, the Shell with Main.qml. The files are edited in the plugin
// directories that RoostdProcess made; roostd does not re-read them.

#include <QDir>
#include <QFile>
#include <QJSEngine>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <chrono>
#include <memory>

#include "api/api.hpp"
#include "api/api_test_support.hpp"
#include "helpers/roostd_process.hpp"
#include "qml_api/roost_singleton.hpp"
#include "roost/plugin_loader.hpp"
#include "roost/plugin_watcher.hpp"
#include "roost/shell.hpp"

using namespace roost;
using namespace roost::api_test;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

using namespace std::chrono_literals;

constexpr auto DEBOUNCE = 50ms;
// A negative check waits this long after the last event it expects.
constexpr int QUIET_MS = 3 * 50 + 100;

// What the panels report to; a global of the engine.
class Probe : public QObject {
    Q_OBJECT

public:
    Q_INVOKABLE void Hit(const QString& tag) { ++Counts[tag]; }
    int Count(const QString& tag) const { return Counts.value(tag); }

    QHash<QString, int> Counts;
};

// A panel: `tag` tells the version of the file, ask() makes a one-shot call
// that reports when it is answered.
QByteArray Panel(const QString& tag, const QString& extra = {}) {
    return QStringLiteral(R"(
import QtQuick
import Roost.Api
Item {
    property string pluginId
    property string tag: "%1"
    %2
    function ask() {
        Roost.store.get(pluginId, function (e, r) {
            roostProbe.Hit("answer:" + pluginId)
        })
    }
}
)")
        .arg(tag, extra)
        .toUtf8();
}

QJsonObject Entry(const QString& id, const QString& qml) {
    return {{"id", id}, {"qml", qml}};
}

QJsonObject PortEntry(const QString& id, const QString& qml,
                      const QJsonObject& inputs, const QJsonObject& outputs) {
    return {{"id", id}, {"qml", qml}, {"inputs", inputs}, {"outputs", outputs}};
}

// A var property as C++ reads it, whichever way it was written.
QVariantMap AsMap(const QVariant& value) {
    if (value.metaType() == QMetaType::fromType<QJSValue>()) {
        return value.value<QJSValue>().toVariant().toMap();
    }
    return value.toMap();
}

QString TagOf(QQuickItem* item) {
    return item == nullptr ? QStringLiteral("(none)")
                           : item->property("tag").toString();
}

}  // namespace

class PluginReloadTest : public QObject {
    Q_OBJECT

private:
    // Plugins first (Daemon_->AddQmlPlugin), then this. Starts roostd, makes
    // the loader and the watcher; the Shell only with `withShell`.
    bool Boot(std::chrono::milliseconds debounce = DEBOUNCE, int maxPaths = 256,
              bool withShell = false) {
        if (!Daemon_->IsValid() || !Daemon_->Start(ROOSTD_PATH)) {
            return false;
        }
        Api_ = std::make_unique<api::Api>();
        Engine_ = std::make_unique<QQmlApplicationEngine>();
        Engine_->globalObject().setProperty("roostProbe",
                                            Engine_->newQObject(&Probe_));
        QJSEngine::setObjectOwnership(&Probe_, QJSEngine::CppOwnership);
        Singleton_ =
            Engine_->singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        if (Singleton_ == nullptr) {
            return false;
        }
        Singleton_->setApi(*Api_);
        QSignalSpy connected(Api_.get(), &api::Api::Connected);
        Api_->Connect(Daemon_->SocketPath());
        if (withShell) {
            Engine_->loadFromModule("Roost.Shell", "Main");
            if (Engine_->rootObjects().isEmpty()) {
                return false;
            }
        }
        if (!connected.wait(TIMEOUT_MS) ||
            !QTest::qWaitFor([&] { return Api_->Plugins().IsLoaded(); },
                             TIMEOUT_MS)) {
            return false;
        }
        Loader_ = std::make_unique<PluginLoader>(*Engine_, Api_->Plugins(),
                                                 *Singleton_);
        if (withShell) {
            Shell_ =
                std::make_unique<Shell>(*Engine_->rootObjects().first(),
                                        *Loader_, *Api_, Daemon_->SocketPath());
        }
        Watcher_ =
            std::make_unique<PluginWatcher>(*Loader_, debounce, maxPaths);
        Reloaded_ = std::make_unique<QSignalSpy>(Watcher_.get(),
                                                 &PluginWatcher::Reloaded);
        return true;
    }

    bool AddPlugin(const QString& id, const QJsonArray& panels,
                   const QMap<QString, QByteArray>& files) {
        return Daemon_->AddQmlPlugin(id, panels, files);
    }

    QString PathOf(const QString& plugin, const QString& file) const {
        return Daemon_->PluginsPath() + "/" + plugin + "/" + file;
    }

    // In place: the file is truncated and written, its inode stays.
    bool EditInPlace(const QString& plugin, const QString& file,
                     const QByteArray& content) {
        QFile out(PathOf(plugin, file));
        return out.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               out.write(content) == content.size();
    }

    // Like an editor: a temporary file, renamed over the original.
    bool EditByRename(const QString& plugin, const QString& file,
                      const QByteArray& content) {
        QSaveFile out(PathOf(plugin, file));
        return out.open(QIODevice::WriteOnly) &&
               out.write(content) == content.size() && out.commit();
    }

    bool WaitReloads(int count) {
        return QTest::qWaitFor([&] { return Reloaded_->count() >= count; },
                               TIMEOUT_MS);
    }

    // Nothing more comes within a few debounces.
    void VerifyNoMoreReloads(int count) {
        QTest::qWait(QUIET_MS);
        QCOMPARE(Reloaded_->count(), count);
    }

    QString LastReloaded() const {
        return Reloaded_->isEmpty() ? QString()
                                    : Reloaded_->last().first().toString();
    }

    // The warning of the loader for a panel that does not load.
    static void ExpectProblemWarning(const QString& plugin) {
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression("^roost: plugin " + plugin + ", panel .*"));
    }

    bool Remove(const QString& plugin, const QString& file) {
        return QFile::remove(PathOf(plugin, file));
    }

    QList<QTimer*> Timers() const {
        return Watcher_->findChildren<QTimer*>(Qt::FindDirectChildrenOnly);
    }

    // The shell part.
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

    std::unique_ptr<roost::testing::RoostdProcess> Daemon_;
    std::unique_ptr<api::Api> Api_;
    Probe Probe_;
    std::unique_ptr<QQmlApplicationEngine> Engine_;
    RoostSingleton* Singleton_ = nullptr;
    std::unique_ptr<PluginLoader> Loader_;
    std::unique_ptr<Shell> Shell_;
    std::unique_ptr<PluginWatcher> Watcher_;
    std::unique_ptr<QSignalSpy> Reloaded_;

private slots:
    void init() {
        Probe_.Counts.clear();
        Daemon_ = std::make_unique<roost::testing::RoostdProcess>();
    }

    void cleanup() {
        // The watcher, then the shell, the loader, the engine.
        Reloaded_.reset();
        Watcher_.reset();
        Shell_.reset();
        Loader_.reset();
        Engine_.reset();
        Singleton_ = nullptr;
        Api_.reset();
        Daemon_.reset();
    }

    void EditingAFileInPlaceReloadsOnlyThatPlugin() {
        QTest::failOnWarning();
        QVERIFY(AddPlugin("a-one", {Entry("one.p", "p.qml")},
                          {{"p.qml", Panel("v1")}}));
        QVERIFY(AddPlugin("b-two", {Entry("two.p", "q.qml")},
                          {{"q.qml", Panel("q1")}}));
        QVERIFY(Boot());
        QQuickItem* one = Loader_->CreatePanel("one.p");
        QQuickItem* two = Loader_->CreatePanel("two.p");
        QVERIFY(one != nullptr && two != nullptr);
        QPointer<QQuickItem> oneGuard = one;
        QPointer<QQuickItem> twoGuard = two;
        QList<QQuickItem*> torn;
        QObject scope;
        connect(
            Loader_.get(), &PluginLoader::PanelAboutToBeDestroyed, &scope,
            [&](QQuickItem* item) { torn.append(item); }, Qt::DirectConnection);
        QSignalSpy types(Loader_.get(), &PluginLoader::PanelTypesChanged);

        QVERIFY(EditInPlace("a-one", "p.qml", Panel("v2")));
        QVERIFY(WaitReloads(1));
        QCOMPARE(LastReloaded(), QString("a-one"));
        VerifyNoMoreReloads(1);

        QCOMPARE(torn, (QList<QQuickItem*>{one}));
        QVERIFY(oneGuard.isNull());
        QVERIFY(!twoGuard.isNull());
        QCOMPARE(types.count(), 1);
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"one.p", "two.p"}));
        QCOMPARE(TagOf(Loader_->CreatePanel("one.p")), QString("v2"));
        QCOMPARE(TagOf(Loader_->CreatePanel("two.p")), QString("q1"));
        QCOMPARE(TagOf(two), QString("q1"));
    }

    void ReplacingAFileByRenameReloadsAndTheNewFileIsWatchedAgain() {
        // On macOS (kqueue) this does not tell whether the watcher re-scans:
        // the path is kept. On Linux (inotify) it does.
        QTest::failOnWarning();
        QVERIFY(AddPlugin("a-one", {Entry("one.p", "p.qml")},
                          {{"p.qml", Panel("v1")}}));
        QVERIFY(Boot());

        QVERIFY(EditByRename("a-one", "p.qml", Panel("v2")));
        QVERIFY(WaitReloads(1));
        QCOMPARE(TagOf(Loader_->CreatePanel("one.p")), QString("v2"));
        QTest::qWait(QUIET_MS);
        const int afterRename = static_cast<int>(Reloaded_->count());

        // In place: no event of the directory, only the file can tell.
        QVERIFY(EditInPlace("a-one", "p.qml", Panel("v3")));
        QVERIFY(WaitReloads(afterRename + 1));
        QCOMPARE(TagOf(Loader_->CreatePanel("one.p")), QString("v3"));
        QTest::qWait(QUIET_MS);
        const int afterInPlace = static_cast<int>(Reloaded_->count());

        QVERIFY(EditByRename("a-one", "p.qml", Panel("v4")));
        QVERIFY(WaitReloads(afterInPlace + 1));
        QCOMPARE(TagOf(Loader_->CreatePanel("one.p")), QString("v4"));
        QTest::qWait(QUIET_MS);
        const int afterSecondRename = static_cast<int>(Reloaded_->count());

        QVERIFY(EditInPlace("a-one", "p.qml", Panel("v5")));
        QVERIFY(WaitReloads(afterSecondRename + 1));
        QCOMPARE(TagOf(Loader_->CreatePanel("one.p")), QString("v5"));
    }

    void ABurstThatLastsLongerThanTheDebounceReloadsOnceAfterTheLastEvent() {
        QTest::failOnWarning();
        QVERIFY(AddPlugin("a-one", {Entry("one.p", "p.qml")},
                          {{"p.qml", Panel("v0")}}));
        // The burst (6 writes, 250 ms apart, 1250 ms) is longer than the
        // debounce, and each gap is shorter than it by a wide margin: macOS
        // delivers the events of a burst late and in groups. The debounce
        // counts from the last event.
        constexpr auto burstDebounce = 1000ms;
        QVERIFY(Boot(burstDebounce));
        QSignalSpy types(Loader_.get(), &PluginLoader::PanelTypesChanged);

        for (int i = 1; i <= 6; ++i) {
            QVERIFY(
                EditInPlace("a-one", "p.qml", Panel(QString("v%1").arg(i))));
            QTest::qWait(250);
        }
        QVERIFY(WaitReloads(1));
        QTest::qWait(burstDebounce + 500ms);
        QCOMPARE(Reloaded_->count(), 1);
        QCOMPARE(types.count(), 1);
        QCOMPARE(TagOf(Loader_->CreatePanel("one.p")), QString("v6"));
    }

    void ANewFileInASubdirectoryIsWatchedAfterTheDirectoryEvent() {
        QTest::failOnWarning();
        const QByteArray helperV1 =
            "import QtQuick\nItem { property string value: \"h1\" }\n";
        const QByteArray helperV2 =
            "import QtQuick\nItem { property string value: \"h2\" }\n";
        QVERIFY(
            AddPlugin("a-one", {Entry("one.p", "p.qml")},
                      {{"p.qml", Panel("v1")}, {"sub/Other.qml", helperV1}}));
        QVERIFY(Boot());

        // The new file shows itself by the event of its directory.
        QVERIFY(EditInPlace("a-one", "sub/Helper.qml", helperV1));
        QVERIFY(WaitReloads(1));
        VerifyNoMoreReloads(1);

        // The panel starts to use it.
        QVERIFY(EditInPlace("a-one", "p.qml",
                            Panel("v2",
                                  "property string helper: h.value\n"
                                  "    Helper { id: h }")
                                .replace("import QtQuick\n",
                                         "import QtQuick\nimport \"sub\"\n")));
        QVERIFY(WaitReloads(2));
        QTest::qWait(QUIET_MS);
        QQuickItem* panel = Loader_->CreatePanel("one.p");
        QVERIFY(panel != nullptr);
        QCOMPARE(panel->property("helper").toString(), QString("h1"));

        // An edit of the new file, in place, reloads: it is watched.
        QVERIFY(EditInPlace("a-one", "sub/Helper.qml", helperV2));
        QVERIFY(WaitReloads(3));
        QQuickItem* fresh = Loader_->CreatePanel("one.p");
        QVERIFY(fresh != nullptr);
        QCOMPARE(fresh->property("helper").toString(), QString("h2"));
    }

    void ABrokenEditMakesThePanelAProblemAndFixingItBringsItBack() {
        QTest::failOnWarning();
        QVERIFY(AddPlugin("a-one", {Entry("one.p", "p.qml")},
                          {{"p.qml", Panel("v1")}}));
        QVERIFY(AddPlugin("b-two", {Entry("two.p", "q.qml")},
                          {{"q.qml", Panel("q1")}}));
        QVERIFY(Boot());
        QQuickItem* two = Loader_->CreatePanel("two.p");
        QPointer<QQuickItem> twoGuard = two;
        QSignalSpy problems(Loader_.get(), &PluginLoader::ProblemsChanged);
        QVERIFY(Loader_->Problems().isEmpty());

        ExpectProblemWarning("a-one");
        QVERIFY(EditInPlace("a-one", "p.qml", "import QtQuick\nItem {{\n"));
        QVERIFY(WaitReloads(1));
        QCOMPARE(Loader_->Problems().size(), 1);
        QCOMPARE(Loader_->Problems().first().Plugin, QString("a-one"));
        QCOMPARE(Loader_->Problems().first().Panel, QString("one.p"));
        QVERIFY2(Loader_->Problems().first().Message.contains("p.qml"),
                 qPrintable(Loader_->Problems().first().Message));
        QCOMPARE(problems.count(), 1);
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"two.p"}));
        QVERIFY(Loader_->CreatePanel("one.p") == nullptr);
        QVERIFY(!twoGuard.isNull());
        QTest::qWait(QUIET_MS);

        QVERIFY(EditInPlace("a-one", "p.qml", Panel("v2")));
        QVERIFY(WaitReloads(2));
        QVERIFY(Loader_->Problems().isEmpty());
        QCOMPARE(problems.count(), 2);
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"one.p", "two.p"}));
        QCOMPARE(TagOf(Loader_->CreatePanel("one.p")), QString("v2"));
    }

    void APendingCallOfAReloadedPanelNeverAnswers() {
        QTest::failOnWarning();
        QVERIFY(AddPlugin("a-one", {Entry("one.p", "p.qml")},
                          {{"p.qml", Panel("v1")}}));
        QVERIFY(AddPlugin("b-two", {Entry("two.p", "q.qml")},
                          {{"q.qml", Panel("q1")}}));
        QVERIFY(Boot());
        QQuickItem* one = Loader_->CreatePanel("one.p");
        QQuickItem* two = Loader_->CreatePanel("two.p");
        QVERIFY(one != nullptr && two != nullptr);

        // The calls leave in this turn, and so does the reload: the answers
        // come after it. The call of the plugin that stays is the control.
        QVERIFY(QMetaObject::invokeMethod(one, "ask"));
        QVERIFY(QMetaObject::invokeMethod(two, "ask"));
        Loader_->Reload("a-one");
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("answer:b-two"), 1, TIMEOUT_MS);
        QTest::qWait(QUIET_MS);
        QCOMPARE(Probe_.Count("answer:a-one"), 0);

        // The new panel of the plugin asks and is answered.
        QQuickItem* fresh = Loader_->CreatePanel("one.p");
        QVERIFY(fresh != nullptr);
        QVERIFY(QMetaObject::invokeMethod(fresh, "ask"));
        QTRY_COMPARE_WITH_TIMEOUT(Probe_.Count("answer:a-one"), 1, TIMEOUT_MS);
    }

    void AfterAReloadOfTheListPluginTheSlotHoldsTheNewPanelAndIsLinked() {
        // No failOnWarning: Main.qml makes Qt warn about fonts.
        const QByteArray terminal = R"(
import QtQuick
Item { property var session: null }
)";
        const auto list = [](const QString& tag) {
            return QStringLiteral(R"(
import QtQuick
Item {
    property string tag: "%1"
    property var selected: null
}
)")
                .arg(tag)
                .toUtf8();
        };
        QVERIFY(AddPlugin("list-plugin",
                          {PortEntry("sessions.list", "list.qml", {},
                                     {{"selected", "SessionRef"}})},
                          {{"list.qml", list("v1")}}));
        QVERIFY(AddPlugin("term-plugin",
                          {PortEntry("terminal.view", "terminal.qml",
                                     {{"session", "SessionRef"}}, {})},
                          {{"terminal.qml", terminal}}));
        QVERIFY(Boot(DEBOUNCE, 256, true));
        QTRY_VERIFY_WITH_TIMEOUT(PanelOf("sessions.list") != nullptr &&
                                     PanelOf("terminal.view") != nullptr,
                                 TIMEOUT_MS);
        QQuickItem* oldList = PanelOf("sessions.list");
        QQuickItem* term = PanelOf("terminal.view");
        QPointer<QQuickItem> oldGuard = oldList;
        QVERIFY(oldList->setProperty(
            "selected", QVariantMap{{"agent", "a"}, {"id", "s1"}}));
        QTRY_COMPARE_WITH_TIMEOUT(AsMap(term->property("session")),
                                  (QVariantMap{{"agent", "a"}, {"id", "s1"}}),
                                  TIMEOUT_MS);

        QVERIFY(EditInPlace("list-plugin", "list.qml", list("v2")));
        QVERIFY(WaitReloads(1));
        QCOMPARE(LastReloaded(), QString("list-plugin"));
        QTRY_VERIFY_WITH_TIMEOUT(TagOf(PanelOf("sessions.list")) == "v2",
                                 TIMEOUT_MS);
        QVERIFY(oldGuard.isNull());
        // The window state of the list is gone; the terminal is the same.
        QVERIFY(
            AsMap(PanelOf("sessions.list")->property("selected")).isEmpty());
        QCOMPARE(PanelOf("terminal.view"), term);
        QCOMPARE(Slot("sessions.list")->property("linkOutput").toString(),
                 QString("selected"));

        QVERIFY(PanelOf("sessions.list")
                    ->setProperty("selected",
                                  QVariantMap{{"agent", "b"}, {"id", "s9"}}));
        QTRY_COMPARE_WITH_TIMEOUT(AsMap(term->property("session")),
                                  (QVariantMap{{"agent", "b"}, {"id", "s9"}}),
                                  TIMEOUT_MS);
    }

    void APluginThatAppearsLaterIsWatched() {
        QTest::failOnWarning();
        QVERIFY(AddPlugin("a-one", {Entry("one.p", "p.qml")},
                          {{"p.qml", Panel("v1")}}));
        QVERIFY(Boot());
        QSignalSpy plugins(Loader_.get(), &PluginLoader::PluginsChanged);

        // roostd reads the plugins at its start: a new one needs a restart.
        QVERIFY(Daemon_->Stop());
        QVERIFY(AddPlugin("b-two", {Entry("two.p", "q.qml")},
                          {{"q.qml", Panel("q1")}}));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QTRY_COMPARE_WITH_TIMEOUT(plugins.count(), 1, TIMEOUT_MS * 2);
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"one.p", "two.p"}));
        QCOMPARE(Reloaded_->count(), 0);

        QVERIFY(EditInPlace("b-two", "q.qml", Panel("q2")));
        QVERIFY(WaitReloads(1));
        QCOMPARE(LastReloaded(), QString("b-two"));
        QCOMPARE(TagOf(Loader_->CreatePanel("two.p")), QString("q2"));
        VerifyNoMoreReloads(1);
    }

    void OverThePathCapAWarningNamesTheUnwatchedAndTheWatchedStillReload() {
        QTest::failOnWarning();
        // Watched: the directory, its parent, a.qml, x.qml. Not: y.qml.
        QVERIFY(AddPlugin("a-one", {Entry("one.p", "a.qml")},
                          {{"a.qml", Panel("v1")},
                           {"x.qml", Panel("x1")},
                           {"y.qml", Panel("y1")}}));
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression("not watched: .*y\\.qml(?!.*(a|x)\\.qml)"));
        QVERIFY(Boot(DEBOUNCE, 4));

        QVERIFY(EditInPlace("a-one", "a.qml", Panel("v2")));
        QVERIFY(WaitReloads(1));
        QCOMPARE(TagOf(Loader_->CreatePanel("one.p")), QString("v2"));
        QTest::qWait(QUIET_MS);
        QVERIFY(EditInPlace("a-one", "x.qml", Panel("x2")));
        QVERIFY(WaitReloads(2));
        QTest::qWait(QUIET_MS);

        QVERIFY(EditInPlace("a-one", "y.qml", Panel("y2")));
        VerifyNoMoreReloads(2);
    }

    void APluginWhoseNameStartsTheNameOfAnotherIsNotItsFile() {
        QTest::failOnWarning();
        QVERIFY(AddPlugin("a-one", {Entry("one.p", "p.qml")},
                          {{"p.qml", Panel("v1")}}));
        QVERIFY(AddPlugin("a-one2", {Entry("one2.p", "p.qml")},
                          {{"p.qml", Panel("w1")}}));
        QVERIFY(Boot());

        QVERIFY(EditInPlace("a-one2", "p.qml", Panel("w2")));
        QVERIFY(WaitReloads(1));
        VerifyNoMoreReloads(1);
        QCOMPARE(LastReloaded(), QString("a-one2"));
        QCOMPARE(TagOf(Loader_->CreatePanel("one.p")), QString("v1"));
        QCOMPARE(TagOf(Loader_->CreatePanel("one2.p")), QString("w2"));
    }

    void ThePendingTimerOfAPluginThatWentAwayIsGone() {
        QTest::failOnWarning();
        QVERIFY(AddPlugin("a-one", {Entry("one.p", "p.qml")},
                          {{"p.qml", Panel("v1")}}));
        QVERIFY(AddPlugin("b-two", {Entry("two.p", "q.qml")},
                          {{"q.qml", Panel("q1")}}));
        // Long: the timer is still pending when the plugin goes.
        QVERIFY(Boot(10s));
        QSignalSpy plugins(Loader_.get(), &PluginLoader::PluginsChanged);

        QVERIFY(EditInPlace("a-one", "p.qml", Panel("v2")));
        QTRY_COMPARE_WITH_TIMEOUT(Timers().size(), 1, TIMEOUT_MS);

        // roostd reads the plugins at its start: a plugin goes with a restart.
        QVERIFY(Daemon_->Stop());
        QVERIFY(QDir(PathOf("a-one", "")).removeRecursively());
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        QTRY_COMPARE_WITH_TIMEOUT(plugins.count(), 1, TIMEOUT_MS * 2);
        QCOMPARE(Loader_->PanelTypes(), (QStringList{"two.p"}));
        // The timer is deleted later (deleteLater).
        QTRY_COMPARE_WITH_TIMEOUT(Timers().size(), 0, TIMEOUT_MS);
        QCOMPARE(Reloaded_->count(), 0);
    }

    void DeletingAFileReloadsOnceAsAProblemAndWritingItAgainReloads() {
        QTest::failOnWarning();
        QVERIFY(AddPlugin("a-one", {Entry("one.p", "p.qml")},
                          {{"p.qml", Panel("v1")}}));
        QVERIFY(Boot());

        ExpectProblemWarning("a-one");
        QVERIFY(Remove("a-one", "p.qml"));
        QVERIFY(WaitReloads(1));
        VerifyNoMoreReloads(1);
        QCOMPARE(Loader_->Problems().size(), 1);
        QCOMPARE(Loader_->Problems().first().Plugin, QString("a-one"));
        QVERIFY(Loader_->CreatePanel("one.p") == nullptr);

        QVERIFY(EditInPlace("a-one", "p.qml", Panel("v2")));
        QVERIFY(WaitReloads(2));
        VerifyNoMoreReloads(2);
        QVERIFY(Loader_->Problems().isEmpty());
        QCOMPARE(TagOf(Loader_->CreatePanel("one.p")), QString("v2"));
    }

    void ADeletedPluginDirectoryMadeAgainLaterReloadsThePlugin() {
        QTest::failOnWarning();
        QVERIFY(AddPlugin("a-one", {Entry("one.p", "p.qml")},
                          {{"p.qml", Panel("v1")}}));
        QVERIFY(Boot());

        ExpectProblemWarning("a-one");
        QVERIFY(QDir(PathOf("a-one", "")).removeRecursively());
        QVERIFY(WaitReloads(1));
        // Later than the debounce: nothing of the plugin is watched now but
        // the directory above it.
        QTest::qWait(QUIET_MS);
        QCOMPARE(Reloaded_->count(), 1);
        QCOMPARE(Loader_->Problems().size(), 1);

        QVERIFY(QDir().mkpath(PathOf("a-one", "")));
        QVERIFY(EditInPlace("a-one", "p.qml", Panel("v2")));
        QVERIFY(WaitReloads(2));
        QCOMPARE(LastReloaded(), QString("a-one"));
        QTest::qWait(QUIET_MS);
        QVERIFY(Loader_->Problems().isEmpty());
        QCOMPARE(TagOf(Loader_->CreatePanel("one.p")), QString("v2"));
        const int afterBack = static_cast<int>(Reloaded_->count());

        // And it is watched again.
        QVERIFY(EditInPlace("a-one", "p.qml", Panel("v3")));
        QVERIFY(WaitReloads(afterBack + 1));
        QCOMPARE(TagOf(Loader_->CreatePanel("one.p")), QString("v3"));
    }

    void ANewTopLevelSubdirectoryWithAFileIsPickedUp() {
        QTest::failOnWarning();
        const QByteArray helperV1 =
            "import QtQuick\nItem { property string value: \"h1\" }\n";
        const QByteArray helperV2 =
            "import QtQuick\nItem { property string value: \"h2\" }\n";
        QVERIFY(AddPlugin("a-one", {Entry("one.p", "p.qml")},
                          {{"p.qml", Panel("v1")}}));
        QVERIFY(Boot());

        QVERIFY(QDir().mkpath(PathOf("a-one", "fresh")));
        QVERIFY(EditInPlace("a-one", "fresh/Helper.qml", helperV1));
        QVERIFY(WaitReloads(1));
        VerifyNoMoreReloads(1);

        // It is watched: an edit in place reloads.
        QVERIFY(EditInPlace("a-one", "fresh/Helper.qml", helperV2));
        QVERIFY(WaitReloads(2));
        VerifyNoMoreReloads(2);
    }

    void EditingQmldirReloads() {
        QTest::failOnWarning();
        QVERIFY(AddPlugin("a-one", {Entry("one.p", "p.qml")},
                          {{"p.qml", Panel("v1")}, {"qmldir", "# v1\n"}}));
        QVERIFY(Boot());

        QVERIFY(EditInPlace("a-one", "qmldir", "# v2\n"));
        QVERIFY(WaitReloads(1));
        QCOMPARE(LastReloaded(), QString("a-one"));
        VerifyNoMoreReloads(1);
    }
};

QTEST_MAIN(PluginReloadTest)
#include "plugin_reload_test.moc"
