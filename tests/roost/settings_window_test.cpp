// The settings window (SettingsWindow.qml, design.md §5), against the real
// roostd: the general section (the document `settings`, edited by patches),
// the settings page of a fixture plugin that a temporary directory holds (its
// document, key `settings`), and the keys, read-only. ROOST_CONFIG is the
// directory of roostd's own temporary tree, so the real settings and keymap
// of the user are never read. No file is opened: the button of keymap.json is
// asked to only say what it would open.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>

#include <memory>

#include "api/api.hpp"
#include "api/api_test_support.hpp"
#include "helpers/combo_popup.hpp"
#include "helpers/roostd_process.hpp"
#include "helpers/scoped_env.hpp"
#include "helpers/snapshot.hpp"
#include "qml_api/roost_singleton.hpp"
#include "qml_api/theme.hpp"
#include "roost/controls_style.hpp"
#include "roost/plugin_loader.hpp"
#include "roost/settings_pages.hpp"

using namespace roost;
using namespace roost::api_test;

// The static plugins of the modules Roost.Api and Roost.Shell, linked into this
// executable.
Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

const QString FIXTURE = QStringLiteral("settings-fixture");

// The page of the fixture plugin: one field, `note`, in the document of the
// plugin under the key `settings`.
const QByteArray FIXTURE_PAGE = R"(
import QtQuick
import QtQuick.Controls
import Roost.Api
Item {
    id: root
    property string pluginId
    implicitHeight: field.implicitHeight
    TextField {
        id: field
        objectName: "fixtureField"
        anchors.fill: parent
        onEditingFinished: Roost.store.patch(root.pluginId, {settings: {note: text}})
    }
    Component.onCompleted: Roost.store.get(root.pluginId, (error, result) => {
        const settings = error ? null : result.document.settings
        if (settings && settings.note !== undefined) {
            field.text = settings.note
        }
    })
}
)";

// A page that compiles but is no Item.
const QByteArray NOT_AN_ITEM_PAGE = "import QtQml\nQtObject {}\n";

// A page that does not compile.
const QByteArray BROKEN_PAGE = "import QtQuick\nItem { nonsense }\n";

// The keymap.json of the tests: a key of the user for a command of the fixture.
const QByteArray KEYMAP =
    R"([{"key": "Ctrl+Alt+H", "command": "settings-fixture.hello"}])"
    "\n";

bool WriteFile(const QString& path, const QByteArray& content) {
    QFile file(path);
    return QDir().mkpath(QFileInfo(path).absolutePath()) &&
           file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(content) == content.size();
}

// A plugin directory with a manifest and files; `settings` is the manifest key
// (nothing when empty).
bool AddPlugin(testing::RoostdProcess& daemon, const QString& id,
               const QString& settings,
               const QMap<QString, QByteArray>& files) {
    QJsonObject contributes{
        {"commands",
         QJsonArray{QJsonObject{{"id", id + ".hello"}, {"title", "Hello"}}}},
        {"keybindings", QJsonArray{QJsonObject{{"command", id + ".hello"},
                                               {"key", "Ctrl+Alt+J"}}}}};
    if (!settings.isEmpty()) {
        contributes.insert("settings", settings);
    }
    const QJsonObject manifest{{"id", id},
                               {"version", "0.1.0"},
                               {"api", "0"},
                               {"contributes", contributes}};
    const QString directory = daemon.PluginsPath() + u'/' + id;
    for (auto it = files.begin(); it != files.end(); ++it) {
        if (!WriteFile(directory + u'/' + it.key(), it.value())) {
            return false;
        }
    }
    return WriteFile(directory + "/plugin.json",
                     QJsonDocument(manifest).toJson());
}

QQuickItem* FindItem(QQuickItem* root, const QString& name) {
    if (root->objectName() == name) {
        return root;
    }
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = FindItem(child, name)) {
            return found;
        }
    }
    return nullptr;
}

void FindItems(QQuickItem* root, const QString& name,
               QList<QQuickItem*>& found) {
    if (root->objectName() == name) {
        found.append(root);
    }
    for (QQuickItem* child : root->childItems()) {
        FindItems(child, name, found);
    }
}

}  // namespace

// One window of roost: its own Api (a connection to roostd), engine, loader and
// the settings window with its pages.
struct SettingsWindow {
    std::unique_ptr<api::Api> Api = std::make_unique<api::Api>();
    std::unique_ptr<QQmlEngine> Engine = std::make_unique<QQmlEngine>();
    RoostSingleton* Singleton = nullptr;
    std::unique_ptr<PluginLoader> Loader;
    std::unique_ptr<QObject> Object;
    std::unique_ptr<SettingsPages> Pages;

    ~SettingsWindow() {
        // As main() does: the pages and the loader before the engine.
        Pages.reset();
        Object.reset();
        Loader.reset();
        Engine.reset();
        Api.reset();
    }

    QQuickWindow* Window() const {
        return qobject_cast<QQuickWindow*>(Object.get());
    }

    QQuickItem* Find(const QString& name) const {
        return FindItem(Window()->contentItem(), name);
    }

    QList<QQuickItem*> FindAll(const QString& name) const {
        QList<QQuickItem*> found;
        FindItems(Window()->contentItem(), name, found);
        return found;
    }

    // The document `settings` as the window has it.
    QJsonObject Document() const {
        return QJsonObject::fromVariantMap(
            Object->property("settingsDocument").toMap());
    }

    // What a user does to a text field: types, then leaves it.
    bool Edit(const QString& field, const QString& text) const {
        QQuickItem* item = Find(field);
        return item != nullptr && item->setProperty("text", text) &&
               QMetaObject::invokeMethod(item, "editingFinished");
    }
};

class SettingsWindowTest : public QObject {
    Q_OBJECT

private:
    // A window against roostd; null if it could not be made. `open`: shown at
    // once. `wait`: made when the connection and the plugins are there; with
    // roostd down it is not waited for.
    std::unique_ptr<SettingsWindow> OpenWindow(bool open = true,
                                               bool wait = true) {
        auto window = std::make_unique<SettingsWindow>();
        QSignalSpy connected(window->Api.get(), &api::Api::Connected);
        window->Singleton = window->Engine->singletonInstance<RoostSingleton*>(
            "Roost.Api", "Roost");
        if (window->Singleton == nullptr) {
            return nullptr;
        }
        window->Singleton->setApi(*window->Api);
        window->Api->Connect(Daemon_->SocketPath());
        if (wait && connected.isEmpty() && !connected.wait(TIMEOUT_MS)) {
            return nullptr;
        }
        if (wait &&
            !QTest::qWaitFor([&] { return window->Api->Plugins().IsLoaded(); },
                             TIMEOUT_MS)) {
            return nullptr;
        }
        window->Loader = std::make_unique<PluginLoader>(
            *window->Engine, window->Api->Plugins(), *window->Singleton);
        QQmlComponent component(window->Engine.get(), "Roost.Shell",
                                "SettingsWindow");
        window->Object.reset(component.create());
        if (window->Object == nullptr || window->Window() == nullptr) {
            qWarning("%s", qPrintable(component.errorString()));
            return nullptr;
        }
        // No file of the user's is opened by a test.
        window->Object->setProperty("opensFiles", false);
        window->Pages =
            std::make_unique<SettingsPages>(*window->Object, *window->Loader);
        if (open) {
            QMetaObject::invokeMethod(window->Object.get(), "open");
        }
        return window;
    }

    bool StartDaemon() { return Daemon_->Start(ROOSTD_PATH); }

    QJsonObject Document(const QString& name) const {
        QFile file(Daemon_->ConfigPath() + u'/' + name + u".json");
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    // The `note` of the fixture's document as the Api of the window has it.
    static QString NoteSeenBy(const SettingsWindow& window) {
        const QFuture<rpc::Result> read = window.Api->Store().Get(FIXTURE);
        if (!WaitFor(read) || !read.result()) {
            return {};
        }
        return read.result()
            ->toObject()
            .value("document")
            .toObject()
            .value("settings")
            .toObject()
            .value("note")
            .toString();
    }

    QString KeymapPath() const {
        return Daemon_->ConfigPath() + "/keymap.json";
    }

    // What a user does to a switch: a click at its middle.
    static void Click(const SettingsWindow& window, QQuickItem* item) {
        // The layout gives the switch its place after the window is shown.
        QTRY_VERIFY(item->width() > 0 && item->height() > 0 &&
                    QRectF(QPointF(0, 0), QSizeF(window.Window()->size()))
                        .contains(item->mapToScene(
                            QPointF(item->width() / 2, item->height() / 2))));
        QTest::mouseClick(
            window.Window(), Qt::LeftButton, Qt::NoModifier,
            item->mapToScene(QPointF(item->width() / 2, item->height() / 2))
                .toPoint());
    }

    std::unique_ptr<testing::RoostdProcess> Daemon_;
    // The window finds keymap.json by ROOST_CONFIG, as roostd does.
    std::unique_ptr<testing::ScopedEnv> Config_;

private slots:
    // The style of the window, as main() selects it, before the first engine.
    void initTestCase() { roost::SelectControlsStyle(); }

    void init() {
        Daemon_ = std::make_unique<testing::RoostdProcess>();
        QVERIFY(Daemon_->IsValid());
        Config_ = std::make_unique<testing::ScopedEnv>(
            "ROOST_CONFIG", Daemon_->ConfigPath().toUtf8());
        QVERIFY(AddPlugin(*Daemon_, FIXTURE, "qml/Settings.qml",
                          {{"qml/Settings.qml", FIXTURE_PAGE}}));
        QVERIFY(WriteFile(KeymapPath(), KEYMAP));
        QVERIFY(StartDaemon());
    }

    // No path of the window writes keymap.json: after every test it is the
    // file the test began with, byte for byte.
    void cleanup() {
        QFile file(KeymapPath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), KEYMAP);
        Daemon_.reset();
        Config_.reset();
    }

    void TheSectionOfThePluginIsShown() {
        auto window = OpenWindow();
        QVERIFY(window);
        QVERIFY(window->Window()->isVisible());
        QTRY_COMPARE(window->FindAll("pluginSettings").size(), 1);
        QQuickItem* section = window->FindAll("pluginSettings").first();
        QCOMPARE(section->property("pluginId").toString(), FIXTURE);
        QQuickItem* field = window->Find("fixtureField");
        QVERIFY(field != nullptr);
        QVERIFY(field->isVisible());
        // The page is in the section, and the section is as tall as the page.
        QVERIFY(field->parentItem()->parentItem() != nullptr);
        QVERIFY(section->height() > field->height());
        QCOMPARE(window->Pages->PluginIds(), QStringList{FIXTURE});
    }

    // The list of a combo box has a row with the name of each entry: the
    // theme combo takes plain strings, which have no role to read.
    void TheListOfThemesShowsTheirNames() {
        auto window = OpenWindow();
        QVERIFY(window);
        QQuickItem* box = window->Find("themeBox");
        QVERIFY(box != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(box->width() > 0 && box->height() > 0,
                                 TIMEOUT_MS);
        QCOMPARE(testing::OpenedRowTexts(window->Window(), box, 1, TIMEOUT_MS),
                 QStringList{"default"});
        QVERIFY(testing::SaveSnapshot(window->Window(), "theme-list"));
    }

    // The ring of the focus is drawn 4 px outside of a control; the scroll
    // area of the window must not cut it, at the first control and at the
    // last one of the form. The pictures are for a person to look at.
    void TheRingOfTheFirstAndTheLastControlIsNotCut() {
        auto window = OpenWindow();
        QVERIFY(window);
        QTRY_VERIFY_WITH_TIMEOUT(!window->FindAll("keymapRow").isEmpty(),
                                 TIMEOUT_MS);
        QQuickItem* scroll = window->Find("settingsScroll");
        QVERIFY(scroll != nullptr);
        // Low enough for the form to scroll, as on a small screen.
        window->Window()->setHeight(300);
        QTRY_VERIFY_WITH_TIMEOUT(scroll->height() < 260, TIMEOUT_MS);
        const auto ringIsInside = [&](QQuickItem* control) {
            constexpr qreal ring = 4;
            const QRectF outer =
                control
                    ->mapRectToScene(
                        QRectF(0, 0, control->width(), control->height()))
                    .adjusted(-ring, -ring, ring, ring);
            const QRectF view = scroll->mapRectToScene(
                QRectF(0, 0, scroll->width(), scroll->height()));
            return view.contains(outer);
        };

        QQuickItem* first = window->Find("themeBox");
        QVERIFY(first != nullptr);
        first->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY_WITH_TIMEOUT(first->property("visualFocus").toBool(),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ringIsInside(first), TIMEOUT_MS);
        QVERIFY(testing::SaveSnapshot(window->Window(), "ring-top"));

        // The last control of the form, scrolled into view to the very end.
        QQuickItem* last = window->Find("openKeymapButton");
        QVERIFY(last != nullptr);
        auto* flickable = scroll->property("contentItem").value<QQuickItem*>();
        QVERIFY(flickable != nullptr);
        QVERIFY(flickable->setProperty(
            "contentY",
            qMax(0.0, flickable->property("contentHeight").toReal() -
                          flickable->height())));
        last->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY_WITH_TIMEOUT(last->property("visualFocus").toBool(),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(ringIsInside(last), TIMEOUT_MS);
        QVERIFY(testing::SaveSnapshot(window->Window(), "ring-bottom"));
    }

    void APluginWithoutAPageHasNoSection() {
        QVERIFY(Daemon_->Stop());
        QVERIFY(AddPlugin(*Daemon_, "plain", QString(), {}));
        QVERIFY(StartDaemon());
        auto window = OpenWindow();
        QVERIFY(window);
        QTRY_COMPARE(window->FindAll("pluginSettings").size(), 1);
        QCOMPARE(window->Pages->PluginIds(), QStringList{FIXTURE});
        QVERIFY(window->Loader->Problems().isEmpty());
    }

    void ABrokenPageIsAProblemAndHasNoSection() {
        QVERIFY(Daemon_->Stop());
        QVERIFY(AddPlugin(*Daemon_, "broken", "qml/Settings.qml",
                          {{"qml/Settings.qml", BROKEN_PAGE}}));
        QVERIFY(StartDaemon());
        auto window = OpenWindow();
        QVERIFY(window);
        QTRY_COMPARE(window->FindAll("pluginSettings").size(), 1);
        QCOMPARE(window->Pages->PluginIds(), QStringList{FIXTURE});
        const auto problems = window->Loader->Problems();
        QCOMPARE(problems.size(), 1);
        QCOMPARE(problems.first().Plugin, QStringLiteral("broken"));
        QCOMPARE(problems.first().Panel, QStringLiteral("(settings)"));
    }

    // The root of a page is no Item: CreateSettingsPage fails and disables the
    // page. That is one problem, no section, and no endless refresh: the
    // signal of the loader is counted, and the pages are cut off from it when
    // it comes too often, so that a loop is a failure and not a hang.
    void APageThatIsNoItemIsOneProblemAndNoLoop() {
        QVERIFY(Daemon_->Stop());
        QVERIFY(AddPlugin(*Daemon_, "noitem", "qml/Settings.qml",
                          {{"qml/Settings.qml", NOT_AN_ITEM_PAGE}}));
        QVERIFY(StartDaemon());
        auto window = OpenWindow(false);
        QVERIFY(window);
        int changes = 0;
        connect(window->Loader.get(), &PluginLoader::SettingsPagesChanged, this,
                [&] {
                    if (++changes == 20) {
                        disconnect(window->Loader.get(),
                                   &PluginLoader::SettingsPagesChanged,
                                   window->Pages.get(), nullptr);
                    }
                });
        QVERIFY(QMetaObject::invokeMethod(window->Object.get(), "open"));
        QTRY_COMPARE(window->FindAll("pluginSettings").size(), 1);
        QVERIFY2(changes < 20, "the pages are refreshed again and again");
        QCOMPARE(window->Pages->PluginIds(), QStringList{FIXTURE});
        auto problems = window->Loader->Problems();
        QCOMPARE(problems.size(), 1);
        QCOMPARE(problems.first().Plugin, QStringLiteral("noitem"));
        QCOMPARE(problems.first().Panel, QStringLiteral("(settings)"));

        // Loaded again, the page fails again: still one problem, no section.
        changes = 0;
        window->Loader->Reload("noitem");
        QVERIFY2(changes < 20, "the pages are refreshed again and again");
        problems = window->Loader->Problems();
        QCOMPARE(problems.size(), 1);
        QCOMPARE(problems.first().Plugin, QStringLiteral("noitem"));
        QCOMPARE(window->Pages->PluginIds(), QStringList{FIXTURE});
        QCOMPARE(window->FindAll("pluginSettings").size(), 1);
    }

    // A page runs the code of its plugin when it is made, so no page is made
    // before the window is shown, also not when a plugin is loaded again.
    void NoPageIsMadeBeforeTheWindowIsShown() {
        auto window = OpenWindow(false);
        QVERIFY(window);
        QVERIFY(!window->Window()->isVisible());
        QCOMPARE(window->Pages->PluginIds(), QStringList());
        QVERIFY(window->Find("fixtureField") == nullptr);
        window->Loader->Reload(FIXTURE);
        QCOMPARE(window->Pages->PluginIds(), QStringList());
        QVERIFY(window->Find("fixtureField") == nullptr);

        QVERIFY(QMetaObject::invokeMethod(window->Object.get(), "open"));
        QCOMPARE(window->Pages->PluginIds(), QStringList{FIXTURE});
        QTRY_VERIFY(window->Find("fixtureField") != nullptr);
    }

    // An edit of the page is in the document of the plugin under `settings`,
    // and outlives the window and roostd.
    void AnEditOfThePageSurvivesTheWindowAndRoostd() {
        {
            auto window = OpenWindow();
            QVERIFY(window);
            QTRY_VERIFY(window->Find("fixtureField") != nullptr);
            QVERIFY(window->Edit("fixtureField", "kept"));
            QTRY_COMPARE(Document(FIXTURE).value("settings").toObject(),
                         (QJsonObject{{"note", "kept"}}));
        }
        // A new window: a new engine, the same roostd.
        {
            auto window = OpenWindow();
            QVERIFY(window);
            QTRY_VERIFY(window->Find("fixtureField") != nullptr);
            QTRY_COMPARE(window->Find("fixtureField")->property("text"),
                         QVariant("kept"));
        }
        // A new roostd on the same directories.
        QVERIFY(Daemon_->Stop());
        QVERIFY(StartDaemon());
        auto window = OpenWindow();
        QVERIFY(window);
        QTRY_VERIFY(window->Find("fixtureField") != nullptr);
        QTRY_COMPARE(window->Find("fixtureField")->property("text"),
                     QVariant("kept"));
    }

    // Reload (the developer mode) destroys the page with the plugin's
    // instances and makes a new one, which reads its values again.
    void APageIsMadeAgainWhenThePluginIsReloaded() {
        auto window = OpenWindow();
        QVERIFY(window);
        QTRY_VERIFY(window->Find("fixtureField") != nullptr);
        QVERIFY(window->Edit("fixtureField", "before"));
        QTRY_COMPARE(Document(FIXTURE).value("settings").toObject(),
                     (QJsonObject{{"note", "before"}}));
        QPointer<QQuickItem> old = window->Find("fixtureField");
        // The window has heard of the write: a new page reads it from there.
        QTRY_COMPARE(NoteSeenBy(*window), QStringLiteral("before"));

        window->Loader->Reload(FIXTURE);
        QTRY_VERIFY(old.isNull());
        QTRY_COMPARE(window->FindAll("pluginSettings").size(), 1);
        QTRY_VERIFY(window->Find("fixtureField") != nullptr);
        QTRY_COMPARE(window->Find("fixtureField")->property("text"),
                     QVariant("before"));
    }

    // The general section is the document `settings`; another window hears
    // the change.
    void AnEditOfTheGeneralSectionIsSeenByAnotherApi() {
        auto window = OpenWindow();
        QVERIFY(window);
        api::Api other;
        QSignalSpy connected(&other, &api::Api::Connected);
        other.Connect(Daemon_->SocketPath());
        QVERIFY(connected.wait(TIMEOUT_MS) || !connected.isEmpty());
        // Changes are heard for the documents that have been read.
        const QFuture<rpc::Result> read = other.Store().Get("settings");
        QVERIFY(WaitFor(read));
        QSignalSpy changed(&other.Store(), &api::StoreService::Changed);

        QVERIFY(window->Edit("uiSizeField", "14"));
        QTRY_VERIFY(!changed.isEmpty());
        QCOMPARE(changed.first().at(0).toString(), QStringLiteral("settings"));
        QCOMPARE(changed.first().at(1).toJsonObject().value("uiFontSize"),
                 QJsonValue(14));
        QCOMPARE(Document("settings"), (QJsonObject{{"uiFontSize", 14}}));
    }

    // Two windows edit different keys before either sees the other's: both
    // stay, also inside one nested object.
    void EditsOfDifferentKeysFromTwoWindowsAreBothKept() {
        auto first = OpenWindow();
        auto second = OpenWindow();
        QVERIFY(first);
        QVERIFY(second);
        QVERIFY(first->Edit("uiSizeField", "14"));
        QVERIFY(first->Edit("terminalFamilyField", "Courier"));
        QVERIFY(second->Edit("terminalSizeField", "15"));
        QTRY_COMPARE(
            Document("settings"),
            (QJsonObject{{"uiFontSize", 14},
                         {"terminalFont",
                          QJsonObject{{"family", "Courier"}, {"size", 15}}}}));
        // Each window shows what the other wrote.
        QTRY_COMPARE(first->Find("terminalSizeField")->property("text"),
                     QVariant("15"));
        QTRY_COMPARE(second->Find("uiSizeField")->property("text"),
                     QVariant("14"));
    }

    // An empty field is the default: the key goes, the others stay.
    void AnEmptyFieldRemovesItsKey() {
        auto window = OpenWindow();
        QVERIFY(window);
        QVERIFY(window->Edit("uiSizeField", "14"));
        QVERIFY(window->Edit("terminalSizeField", "15"));
        QTRY_COMPARE(Document("settings").value("uiFontSize"), QJsonValue(14));
        // The window has heard of it: an edit is the difference from that.
        QTRY_COMPARE(window->Document().value("uiFontSize"), QJsonValue(14));
        QVERIFY(window->Edit("uiSizeField", ""));
        QTRY_VERIFY(!Document("settings").contains("uiFontSize"));
        QCOMPARE(Document("settings").value("terminalFont").toObject(),
                 (QJsonObject{{"size", 15}}));
    }

    // The last key of the terminal font that goes takes the object with it.
    void TheLastKeyOfTheTerminalFontTakesTheObjectWithIt() {
        auto window = OpenWindow();
        QVERIFY(window);
        QVERIFY(window->Edit("terminalSizeField", "15"));
        QVERIFY(window->Edit("terminalFamilyField", "Courier"));
        QTRY_COMPARE(Document("settings").value("terminalFont").toObject(),
                     (QJsonObject{{"family", "Courier"}, {"size", 15}}));
        QTRY_COMPARE(window->Document().value("terminalFont").toObject().size(),
                     2);
        QVERIFY(window->Edit("terminalSizeField", ""));
        QTRY_COMPARE(Document("settings").value("terminalFont").toObject(),
                     (QJsonObject{{"family", "Courier"}}));
        QTRY_COMPARE(window->Document().value("terminalFont").toObject().size(),
                     1);
        QVERIFY(window->Edit("terminalFamilyField", ""));
        QTRY_VERIFY(!Document("settings").contains("terminalFont"));
    }

    // A write that fails leaves no wrong value in the field, and the reason
    // stays on the screen.
    void AFailedWriteReturnsTheFieldAndShowsTheError() {
        auto window = OpenWindow();
        QVERIFY(window);
        QVERIFY(window->Edit("uiSizeField", "14"));
        QTRY_COMPARE(window->Document().value("uiFontSize"), QJsonValue(14));
        QVERIFY(Daemon_->Stop());
        QTRY_VERIFY(!window->Singleton->connected());

        QVERIFY(window->Edit("uiSizeField", "20"));
        QTRY_COMPARE(window->Find("uiSizeField")->property("text"),
                     QVariant("14"));
        QTRY_VERIFY(!window->Object->property("error").toString().isEmpty());
        QQuickItem* label = window->Find("settingsError");
        QVERIFY(label != nullptr);
        QVERIFY(label->isVisible());
        QVERIFY(label->property("text").toString().contains(
            window->Object->property("error").toString()));
    }

    // The switch of the update check is one key of one patch, as the fields
    // are; the other keys stay.
    void TheUpdateSwitchWritesItsKey() {
        auto window = OpenWindow();
        QVERIFY(window);
        QQuickItem* check = window->Find("updatesCheckSwitch");
        QVERIFY(check != nullptr);
        // No key is the default: on.
        QVERIFY(check->property("checked").toBool());
        QVERIFY(window->Edit("uiSizeField", "14"));
        QTRY_COMPARE(window->Document().value("uiFontSize"), QJsonValue(14));

        Click(*window, check);
        QTRY_COMPARE(Document("settings"),
                     (QJsonObject{{"uiFontSize", 14},
                                  {"updates", QJsonObject{{"check", false}}}}));
        QTRY_VERIFY(!check->property("checked").toBool());
        // Another window hears it, and shows it.
        auto other = OpenWindow();
        QVERIFY(other);
        QTRY_VERIFY(
            !other->Find("updatesCheckSwitch")->property("checked").toBool());

        Click(*window, check);
        QTRY_COMPARE(Document("settings").value("updates"),
                     QJsonValue(QJsonObject{{"check", true}}));
        QTRY_VERIFY(check->property("checked").toBool());
        QTRY_VERIFY(
            other->Find("updatesCheckSwitch")->property("checked").toBool());
        QCOMPARE(Document("settings").value("uiFontSize"), QJsonValue(14));
    }

    // The switch is a track and a knob in the tokens: `primary` and the knob
    // at the right when on, `textMuted` and the knob at the left when off.
    // Derived, no mockup (docs/design/18-style.md).
    void TheUpdateSwitchIsDrawnInTheTokens() {
        auto window = OpenWindow();
        QVERIFY(window);
        QQuickItem* check = window->Find("updatesCheckSwitch");
        QVERIFY(check != nullptr);
        QQuickItem* track = window->Find("switchTrack");
        QQuickItem* knob = window->Find("switchKnob");
        QVERIFY(track != nullptr && knob != nullptr);
        const auto color = [](const QQuickItem* item) {
            return item->property("color").value<QColor>();
        };
        const Theme* theme = window->Singleton->theme();
        QTRY_VERIFY_WITH_TIMEOUT(track->width() > 0, TIMEOUT_MS);
        QVERIFY(check->property("checked").toBool());
        QCOMPARE(color(track), theme->primary());
        QCOMPARE(color(knob), theme->surface());
        QCOMPARE(knob->x(), track->width() - knob->width() - 2);
        QVERIFY(testing::SaveSnapshot(window->Window(), "switch-on"));

        Click(*window, check);
        QTRY_VERIFY_WITH_TIMEOUT(!check->property("checked").toBool(),
                                 TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(color(track), theme->textMuted(), TIMEOUT_MS);
        QCOMPARE(color(knob), theme->surface());
        QTRY_COMPARE_WITH_TIMEOUT(knob->x(), 2.0, TIMEOUT_MS);
        QVERIFY(testing::SaveSnapshot(window->Window(), "switch-off"));
    }

    void TheUpdateSwitchShowsTheStoredKey() {
        QVERIFY(Daemon_->Stop());
        QVERIFY(WriteFile(Daemon_->ConfigPath() + "/settings.json",
                          R"({"updates": {"check": false}})"));
        QVERIFY(StartDaemon());
        auto window = OpenWindow();
        QVERIFY(window);
        QTRY_VERIFY(
            !window->Find("updatesCheckSwitch")->property("checked").toBool());
    }

    // A write that fails returns the switch to the stored value and shows the
    // reason, as for the fields.
    void AFailedWriteReturnsTheUpdateSwitch() {
        auto window = OpenWindow();
        QVERIFY(window);
        QQuickItem* check = window->Find("updatesCheckSwitch");
        QVERIFY(check != nullptr);
        QVERIFY(window->Edit("uiSizeField", "14"));
        QTRY_COMPARE(window->Document().value("uiFontSize"), QJsonValue(14));
        QVERIFY(Daemon_->Stop());
        QTRY_VERIFY(!window->Singleton->connected());

        Click(*window, check);
        QTRY_VERIFY(!window->Object->property("error").toString().isEmpty());
        QTRY_VERIFY(check->property("checked").toBool());
        QVERIFY(window->Find("settingsError")->isVisible());
    }

    // Opened while roostd is down: the window says why it is empty, and shows
    // the stored values when it is up and connected.
    void AWindowOpenedBeforeRoostdIsUpReadsWhenItConnects() {
        QVERIFY(Daemon_->Stop());
        QVERIFY(
            WriteFile(Daemon_->ConfigPath() + "/settings.json",
                      R"({"uiFontSize": 14, "terminalFont": {"size": 15}})"));
        auto window = OpenWindow(true, false);
        QVERIFY(window);
        QTRY_VERIFY(window->Find("settingsError")->isVisible());
        QCOMPARE(window->Find("uiSizeField")->property("text"), QVariant(""));

        QVERIFY(StartDaemon());
        QTRY_COMPARE(window->Find("uiSizeField")->property("text"),
                     QVariant("14"));
        QCOMPARE(window->Find("terminalSizeField")->property("text"),
                 QVariant("15"));
        QTRY_VERIFY(!window->Find("settingsError")->isVisible());
        QTRY_COMPARE(window->FindAll("keymapRow").size(), 2);
    }

    // Text that is no number is no edit: the field goes back to the value.
    void ATextThatIsNoNumberIsNotWritten() {
        auto window = OpenWindow();
        QVERIFY(window);
        QVERIFY(window->Edit("uiSizeField", "14"));
        QTRY_COMPARE(Document("settings").value("uiFontSize"), QJsonValue(14));
        QTRY_COMPARE(window->Document().value("uiFontSize"), QJsonValue(14));
        QVERIFY(window->Edit("uiSizeField", "big"));
        QCOMPARE(window->Find("uiSizeField")->property("text"), QVariant("14"));
        QCOMPARE(Document("settings").value("uiFontSize"), QJsonValue(14));
    }

    // The keys: the table of roostd, read-only, with the user's file and the
    // plugin's key among them; the button only says which file it opens.
    void TheKeysAreListedAndTheFileIsOnlyOffered() {
        auto window = OpenWindow();
        QVERIFY(window);
        QTRY_COMPARE(window->FindAll("keymapRow").size(), 2);
        QStringList rows;
        for (QQuickItem* row : window->FindAll("keymapRow")) {
            QStringList texts;
            for (QQuickItem* child : row->childItems()) {
                texts.append(child->property("text").toString());
            }
            rows.append(texts.join("|"));
        }
        rows.sort();
        QCOMPARE(rows, (QStringList{"settings-fixture.hello|Ctrl+Alt+H|user",
                                    "settings-fixture.hello|Ctrl+Alt+J|"
                                    "settings-fixture"}));

        QSignalSpy requested(window->Object.get(),
                             SIGNAL(keymapFileRequested(QUrl)));
        QQuickItem* button = window->Find("openKeymapButton");
        QVERIFY(button != nullptr);
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QCOMPARE(requested.size(), 1);
        QCOMPARE(requested.first().at(0).toUrl(),
                 QUrl::fromLocalFile(KeymapPath()));
    }

    // Main.qml: the command shell.settings, the toolbar button (the menu item
    // on macOS) show the one window, and again the same one.
    void TheCommandAndTheButtonOpenTheOneWindow() {
        api::Api api;
        QQmlApplicationEngine engine;
        auto* singleton =
            engine.singletonInstance<RoostSingleton*>("Roost.Api", "Roost");
        QVERIFY(singleton != nullptr);
        singleton->setApi(api);
        engine.loadFromModule("Roost.Shell", "Main");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* main = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        QVERIFY(main != nullptr);
        auto* settings = main->findChild<QQuickWindow*>("settingsWindow");
        QVERIFY(settings != nullptr);
        settings->setProperty("opensFiles", false);
        QVERIFY(!settings->isVisible());

        const QFuture<rpc::Result> run = api.Commands().Run("shell.settings");
        QVERIFY(WaitFor(run));
        QVERIFY2(run.result(), qPrintable(Why(run.result())));
        QVERIFY(settings->isVisible());

        settings->hide();
#ifdef Q_OS_MACOS
        QObject* item = main->findChild<QObject*>("settingsMenuItem");
        QVERIFY(item != nullptr);
        QCOMPARE(item->property("text").toString(),
                 QString::fromUtf8("Settings…"));
        QVERIFY(QMetaObject::invokeMethod(item, "triggered"));
#else
        auto* button = main->findChild<QQuickItem*>("settingsButton");
        QVERIFY(button != nullptr);
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
#endif
        QVERIFY(settings->isVisible());
        // Open again while it is shown: the same window.
        QVERIFY(QMetaObject::invokeMethod(main, "openSettings"));
        QCOMPARE(main->findChildren<QQuickWindow*>("settingsWindow").size(), 1);
        QVERIFY(settings->isVisible());
    }
};

QTEST_MAIN(SettingsWindowTest)
#include "settings_window_test.moc"
