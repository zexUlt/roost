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

#include <algorithm>
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
#include "terminal/renderer.hpp"

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

// The first entry of the terminal family box.
QString DefaultFamilyEntry(const Theme& theme) {
    return QStringLiteral("System monospace (%1)")
        .arg(theme.defaultTerminalFont().family());
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

    // What a user does to the box of the terminal family: types a name, then
    // leaves it.
    bool TypeFamily(const QString& text) const {
        QQuickItem* box = Find("terminalFamilyBox");
        auto* input = box == nullptr
                          ? nullptr
                          : box->property("contentItem").value<QQuickItem*>();
        return input != nullptr && input->setProperty("text", text) &&
               QMetaObject::invokeMethod(input, "editingFinished");
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

    // A second client that hears every change of the document `settings`: the
    // patches the window wrote. Null if it did not connect.
    std::unique_ptr<api::Api> Listen() {
        auto other = std::make_unique<api::Api>();
        QSignalSpy connected(other.get(), &api::Api::Connected);
        other->Connect(Daemon_->SocketPath());
        if (!(connected.wait(TIMEOUT_MS) || !connected.isEmpty())) {
            return nullptr;
        }
        const QFuture<rpc::Result> read = other->Store().Get("settings");
        return WaitFor(read) ? std::move(other) : nullptr;
    }

    // Keys of the text, one after another, to the focus of the window.
    static void TypeText(QQuickWindow* window, const QString& text) {
        for (const QChar ch : text) {
            QTest::keyClick(window, ch.toLatin1());
        }
    }

    // The next write of the window is a barrier: whatever the window wrote
    // before it has been heard when this returns. The result is how many
    // writes were heard before the barrier, -1 if the barrier was not heard.
    static qsizetype WritesBeforeBarrier(const SettingsWindow& window,
                                         const QSignalSpy& changed, int size) {
        const qsizetype before = changed.size();
        if (!window.Edit("uiSizeField", QString::number(size)) ||
            !QTest::qWaitFor([&] { return changed.size() > before; },
                             TIMEOUT_MS)) {
            return -1;
        }
        return changed.size() - 1;
    }

    // The window is on its family box with the keyboard, as after a click in
    // its text. The text is selected, so that the typing replaces it.
    static QQuickItem* FocusFamilyText(const SettingsWindow& window) {
        QQuickItem* box = window.Find("terminalFamilyBox");
        if (box == nullptr ||
            !QTest::qWaitFor([&] { return box->width() > 0; }, TIMEOUT_MS)) {
            return nullptr;
        }
        QTest::mouseClick(
            window.Window(), Qt::LeftButton, Qt::NoModifier,
            box->mapToScene(QPointF(box->width() / 3, box->height() / 2))
                .toPoint());
        auto* input = box->property("contentItem").value<QQuickItem*>();
        if (input == nullptr ||
            !QTest::qWaitFor([&] { return input->hasActiveFocus(); },
                             TIMEOUT_MS)) {
            return nullptr;
        }
        QMetaObject::invokeMethod(input, "selectAll");
        return box;
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

    // What a user does to a control: Tab to it, which scrolls the form to it
    // when it is below the visible part (what a font of another size does to
    // the layout), and a click at its middle.
    static void Click(const SettingsWindow& window, QQuickItem* item) {
        // The layout gives the control its place after the window is shown.
        QTRY_VERIFY(item->width() > 0 && item->height() > 0);
        item->forceActiveFocus(Qt::TabFocusReason);
        QQuickItem* scroll = window.Find("settingsScroll");
        QVERIFY(scroll != nullptr);
        const auto middle = [&] {
            return item->mapToScene(
                QPointF(item->width() / 2, item->height() / 2));
        };
        QTRY_VERIFY(scroll
                        ->mapRectToScene(
                            QRectF(0, 0, scroll->width(), scroll->height()))
                        .contains(middle()));
        QTest::mouseClick(window.Window(), Qt::LeftButton, Qt::NoModifier,
                          middle().toPoint());
    }

    // What another program, or a hand edit, leaves in the document: a patch
    // that the window never writes itself.
    static bool PatchSettings(const SettingsWindow& window,
                              const QJsonObject& patch) {
        const QFuture<rpc::Result> writing =
            window.Api->Store().Patch(QStringLiteral("settings"), patch);
        return WaitFor(writing) && writing.result();
    }

    static QString InUseText(const QFont& font) {
        return "In use: " + font.family() + " / " + Theme::sizeText(font);
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

    // The form scrolls: a control below the visible part is reached with the
    // keyboard, which brings it into view with its ring, and then with the
    // mouse.
    void AControlBelowTheVisiblePartIsReached() {
        auto window = OpenWindow();
        QVERIFY(window);
        QTRY_VERIFY_WITH_TIMEOUT(!window->FindAll("keymapRow").isEmpty(),
                                 TIMEOUT_MS);
        QQuickItem* scroll = window->Find("settingsScroll");
        QQuickItem* check = window->Find("updatesCheckSwitch");
        QVERIFY(scroll != nullptr && check != nullptr);
        window->Window()->setHeight(300);
        QTRY_VERIFY_WITH_TIMEOUT(scroll->height() < 260, TIMEOUT_MS);
        const auto view = [&] {
            return scroll->mapRectToScene(
                QRectF(0, 0, scroll->width(), scroll->height()));
        };
        const auto outer = [&] {
            constexpr qreal ring = 4;
            return check
                ->mapRectToScene(QRectF(0, 0, check->width(), check->height()))
                .adjusted(-ring, -ring, ring, ring);
        };
        // Below the visible part to begin with.
        QVERIFY(!view().contains(outer()));

        check->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY_WITH_TIMEOUT(view().contains(outer()), TIMEOUT_MS);
        Click(*window, check);
        QTRY_VERIFY(
            Document("settings").value("updates").toObject().contains("check"));
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
        QVERIFY(first->TypeFamily("Courier"));
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

    // The text of the family box has the keyboard and nothing is typed while
    // another window writes a family: leaving the box writes nothing back, and
    // the box shows the new family.
    void LeavingAnUntouchedFamilyTextKeepsAChangeFromElsewhere() {
        auto first = OpenWindow();
        auto second = OpenWindow();
        QVERIFY(first);
        QVERIFY(second);
        QQuickItem* box = first->Find("terminalFamilyBox");
        QVERIFY(box != nullptr);
        auto* input = box->property("contentItem").value<QQuickItem*>();
        QVERIFY(input != nullptr);
        // The second window was opened last and has the keyboard.
        box->window()->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(box->window()));
        input->forceActiveFocus();
        QTRY_VERIFY(input->hasActiveFocus());
        QVERIFY(second->TypeFamily("Courier"));
        QTRY_COMPARE(Document("settings").value("terminalFont").toObject(),
                     (QJsonObject{{"family", "Courier"}}));
        QTRY_COMPARE(
            first->Document().value("terminalFont").toObject().value("family"),
            QJsonValue("Courier"));
        QVERIFY(input->hasActiveFocus());
        QVERIFY(QMetaObject::invokeMethod(input, "editingFinished"));
        // A later write of the first window is the barrier: a write of the
        // family back would come before it.
        QVERIFY(first->Edit("uiSizeField", "14"));
        QTRY_COMPARE(Document("settings").value("uiFontSize"), QJsonValue(14));
        QCOMPARE(Document("settings").value("terminalFont").toObject(),
                 (QJsonObject{{"family", "Courier"}}));
        QTRY_VERIFY(box->property("currentText")
                        .toString()
                        .startsWith(QStringLiteral("Courier")));
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
        QVERIFY(window->TypeFamily("Courier"));
        QTRY_COMPARE(Document("settings").value("terminalFont").toObject(),
                     (QJsonObject{{"family", "Courier"}, {"size", 15}}));
        QTRY_COMPARE(window->Document().value("terminalFont").toObject().size(),
                     2);
        QVERIFY(window->Edit("terminalSizeField", ""));
        QTRY_COMPARE(Document("settings").value("terminalFont").toObject(),
                     (QJsonObject{{"family", "Courier"}}));
        QTRY_COMPARE(window->Document().value("terminalFont").toObject().size(),
                     1);
        QVERIFY(window->TypeFamily(""));
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

    // The ring of the keyboard focus is around the track, outside with a gap
    // of 2 px, not around the whole control with its padding.
    void TheFocusRingOfTheSwitchIsAroundTheTrack() {
        auto window = OpenWindow();
        QVERIFY(window);
        QQuickItem* check = window->Find("updatesCheckSwitch");
        QQuickItem* track = window->Find("switchTrack");
        QQuickItem* ring = window->Find("switchRing");
        QVERIFY(check != nullptr && track != nullptr && ring != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(track->width() > 0, TIMEOUT_MS);
        QVERIFY(!ring->isVisible());
        check->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY_WITH_TIMEOUT(ring->isVisible(), TIMEOUT_MS);
        const double grow = 2 + window->Singleton->theme()->cursorRingWidth();
        const QRectF expected =
            track->mapRectToScene(QRectF(0, 0, track->width(), track->height()))
                .adjusted(-grow, -grow, grow, grow);
        QCOMPARE(
            ring->mapRectToScene(QRectF(0, 0, ring->width(), ring->height())),
            expected);
        QVERIFY(testing::SaveSnapshot(window->Window(), "switch-focused"));
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

    // An empty size field says which size is in force, from the theme and not
    // from a literal; a stored size is the text and has a Reset.
    void TheSizeFieldsSayTheSizeInForce() {
        auto window = OpenWindow();
        QVERIFY(window);
        const Theme* theme = window->Singleton->theme();
        const auto sizeText = [](const QFont& font) {
            return Theme::sizeText(font);
        };
        QQuickItem* terminal = window->Find("terminalSizeField");
        QQuickItem* ui = window->Find("uiSizeField");
        QQuickItem* reset = window->Find("terminalSizeReset");
        QVERIFY(terminal != nullptr && ui != nullptr && reset != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(
            terminal->property("placeholderText"),
            QVariant("default: " + sizeText(theme->defaultTerminalFont())),
            TIMEOUT_MS);
        QCOMPARE(ui->property("placeholderText"),
                 QVariant("default: " + sizeText(theme->defaultUiFont())));
        QVERIFY(!reset->isVisible());

        // A size that no system font has by default.
        QVERIFY(window->Edit("terminalSizeField", "27.5"));
        QTRY_COMPARE_WITH_TIMEOUT(theme->terminalFont().pointSizeF(), 27.5,
                                  TIMEOUT_MS);
        QCOMPARE(terminal->property("text"), QVariant("27.5"));
        QTRY_VERIFY_WITH_TIMEOUT(reset->isVisible(), TIMEOUT_MS);
        QCOMPARE(ui->property("placeholderText"),
                 QVariant("default: " + sizeText(theme->defaultUiFont())));

        // Reset is the empty field: the key goes.
        Click(*window, reset);
        QTRY_VERIFY_WITH_TIMEOUT(!Document("settings").contains("terminalFont"),
                                 TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(terminal->property("text"), QVariant(""),
                                  TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!reset->isVisible(), TIMEOUT_MS);
        QCOMPARE(
            terminal->property("placeholderText"),
            QVariant("default: " + sizeText(theme->defaultTerminalFont())));
    }

    // A size is said as the font has it: points without a trailing ".0", and
    // pixels, with "px", when the font has no point size. The QML side cannot
    // tell them apart (it has both for any font), so the text is made in C++.
    void ASizeIsSaidInPointsOrPixels() {
        QFont points;
        points.setPointSizeF(13);
        QCOMPARE(Theme::sizeText(points), QString("13 pt"));
        points.setPointSizeF(13.5);
        QCOMPARE(Theme::sizeText(points), QString("13.5 pt"));
        QFont pixels;
        pixels.setPixelSize(17);
        QCOMPARE(Theme::sizeText(pixels), QString("17 px"));

        // QML calls it by its name.
        auto window = OpenWindow();
        QVERIFY(window);
        QString text;
        QVERIFY(QMetaObject::invokeMethod(
            window->Singleton->theme(), "sizeText", Q_RETURN_ARG(QString, text),
            Q_ARG(QFont, pixels)));
        QCOMPARE(text, QString("17 px"));
    }

    // A family that is not installed stays in the box with its reason, and
    // the first entry is the font that the terminals use.
    void ANotInstalledFamilyKeepsItsTextAndTheFirstEntryIsTheFontInForce() {
        auto window = OpenWindow();
        QVERIFY(window);
        const Theme* theme = window->Singleton->theme();
        const QString missing = "Roost No Such Family";
        QQuickItem* family = window->Find("terminalFamilyBox");
        QQuickItem* problems = window->Find("terminalFontProblems");
        QVERIFY(family != nullptr && problems != nullptr);
        QVERIFY(window->TypeFamily(missing));
        QTRY_VERIFY_WITH_TIMEOUT(problems->isVisible(), TIMEOUT_MS);
        QVERIFY(problems->property("text").toString().contains(missing));
        QTRY_COMPARE_WITH_TIMEOUT(family->property("currentText"),
                                  QVariant(missing + " (not installed)"),
                                  TIMEOUT_MS);
        QVERIFY(theme->terminalFont().family() != missing);
        // The first entry is still the default, not the stored name.
        QCOMPARE(family->property("model").toStringList().first(),
                 DefaultFamilyEntry(*theme));
        QVERIFY(window->Find("terminalFamilyReset")->isVisible());
    }

    // The list of the family box: the default first, then the installed
    // monospace families; it is made when the box opens, not before.
    void TheFamilyListHasTheDefaultFirstThenTheInstalledMonospaceFamilies() {
        auto window = OpenWindow();
        QVERIFY(window);
        const Theme* theme = window->Singleton->theme();
        QQuickItem* box = window->Find("terminalFamilyBox");
        QVERIFY(box != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(box->width() > 0 && box->height() > 0,
                                 TIMEOUT_MS);
        // The CI image has one monospace family: the rows are the default and
        // as many of the installed ones as there are, up to two.
        const QStringList installed = InstalledMonospaceFamilies();
        if (installed.isEmpty()) {
            QSKIP("no fixed-pitch font is installed");
        }
        const qsizetype shown = std::min<qsizetype>(2, installed.size());
        // Closed: the default only, and the box shows it.
        QCOMPARE(box->property("count"), QVariant(1));
        QCOMPARE(box->property("currentText"),
                 QVariant(DefaultFamilyEntry(*theme)));

        QCOMPARE(testing::OpenedRowTexts(window->Window(), box,
                                         static_cast<int>(1 + shown),
                                         TIMEOUT_MS, true),
                 (QStringList{DefaultFamilyEntry(*theme)} +
                  installed.mid(0, shown)));
        QCOMPARE(box->property("count"), QVariant(1 + installed.size()));
        QCOMPARE(box->property("model").toStringList().mid(1), installed);
        QVERIFY(testing::SaveSnapshot(window->Window(), "family-list"));
    }

    // A family picked from the list is one `store.patch` of the one key.
    void PickingAFamilyWritesOneKey() {
        auto window = OpenWindow();
        QVERIFY(window);
        QQuickItem* box = window->Find("terminalFamilyBox");
        QVERIFY(box != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(box->width() > 0 && box->height() > 0,
                                 TIMEOUT_MS);
        QCOMPARE(
            testing::OpenedRowTexts(window->Window(), box, 1, TIMEOUT_MS, true)
                .size(),
            1);
        const QString family = InstalledMonospaceFamilies().at(0);

        api::Api other;
        QSignalSpy connected(&other, &api::Api::Connected);
        other.Connect(Daemon_->SocketPath());
        QVERIFY(connected.wait(TIMEOUT_MS) || !connected.isEmpty());
        const QFuture<rpc::Result> read = other.Store().Get("settings");
        QVERIFY(WaitFor(read));
        QSignalSpy changed(&other.Store(), &api::StoreService::Changed);

        QVERIFY(QMetaObject::invokeMethod(box, "activated", Q_ARG(int, 1)));
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), TIMEOUT_MS);
        QCOMPARE(
            changed.first().at(1).toJsonObject(),
            (QJsonObject{{"terminalFont", QJsonObject{{"family", family}}}}));
        // The next write of the window is a barrier: nothing came in between.
        QVERIFY(window->Edit("uiSizeField", "14"));
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 2, TIMEOUT_MS);
        QCOMPARE(changed.at(1).at(1).toJsonObject().value("uiFontSize"),
                 QJsonValue(14));
        QCOMPARE(Document("settings").value("terminalFont").toObject(),
                 (QJsonObject{{"family", family}}));
        // The list is still open (the signal was sent by hand): the box shows
        // the family when it closes.
        QTest::keyClick(window->Window(), Qt::Key_Escape);
        QTRY_COMPARE_WITH_TIMEOUT(box->property("currentText"),
                                  QVariant(family), TIMEOUT_MS);
    }

    // The first entry is the default: it removes the key, and the size stays.
    void TheFirstEntryOfTheFamilyListRemovesTheKey() {
        auto window = OpenWindow();
        QVERIFY(window);
        const Theme* theme = window->Singleton->theme();
        QQuickItem* box = window->Find("terminalFamilyBox");
        QVERIFY(box != nullptr);
        QVERIFY(window->Edit("terminalSizeField", "15"));
        QVERIFY(window->TypeFamily(InstalledMonospaceFamilies().at(0)));
        QTRY_COMPARE_WITH_TIMEOUT(
            window->Document().value("terminalFont").toObject().size(), 2,
            TIMEOUT_MS);
        QVERIFY(QMetaObject::invokeMethod(box, "activated", Q_ARG(int, 0)));
        QTRY_COMPARE_WITH_TIMEOUT(
            Document("settings").value("terminalFont").toObject(),
            (QJsonObject{{"size", 15}}), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(box->property("currentText"),
                                  QVariant(DefaultFamilyEntry(*theme)),
                                  TIMEOUT_MS);
        // The default entry as typed text does the same.
        QVERIFY(window->TypeFamily(InstalledMonospaceFamilies().at(0)));
        QTRY_COMPARE_WITH_TIMEOUT(
            window->Document().value("terminalFont").toObject().size(), 2,
            TIMEOUT_MS);
        QVERIFY(window->TypeFamily(DefaultFamilyEntry(*theme)));
        QTRY_COMPARE_WITH_TIMEOUT(
            Document("settings").value("terminalFont").toObject(),
            (QJsonObject{{"size", 15}}), TIMEOUT_MS);
    }

    // A stored family that the list does not have is shown, marked, and an
    // installed one is shown as itself: the box never shows another entry.
    void AStoredFamilyIsShownAndAMissingOneIsMarked() {
        auto window = OpenWindow();
        QVERIFY(window);
        QQuickItem* box = window->Find("terminalFamilyBox");
        QVERIFY(box != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(box->width() > 0 && box->height() > 0,
                                 TIMEOUT_MS);
        const QString installed = InstalledMonospaceFamilies().at(0);
        const QString missing = "Roost No Such Family";

        QVERIFY(PatchSettings(
            *window, {{"terminalFont", QJsonObject{{"family", installed}}}}));
        QTRY_COMPARE_WITH_TIMEOUT(box->property("currentText"),
                                  QVariant(installed), TIMEOUT_MS);
        QVERIFY(PatchSettings(
            *window, {{"terminalFont", QJsonObject{{"family", missing}}}}));
        QTRY_COMPARE_WITH_TIMEOUT(box->property("currentText"),
                                  QVariant(missing + " (not installed)"),
                                  TIMEOUT_MS);
        // The list has it, after the default, and the box is on it.
        QCOMPARE(box->property("model").toStringList().mid(0, 2),
                 (QStringList{DefaultFamilyEntry(*window->Singleton->theme()),
                              missing + " (not installed)"}));
        QCOMPARE(box->property("currentIndex"), QVariant(1));
        // Enter on the marked text keeps the stored name, not the mark.
        QVERIFY(window->TypeFamily(missing + " (not installed)"));
        QVERIFY(PatchSettings(*window, {{"uiFontSize", 14}}));
        QTRY_COMPARE_WITH_TIMEOUT(window->Document().value("uiFontSize"),
                                  QJsonValue(14), TIMEOUT_MS);
        QCOMPARE(Document("settings").value("terminalFont").toObject(),
                 (QJsonObject{{"family", missing}}));
    }

    // Clicking a row of the opened list is one patch, although the box says
    // both `activated` and `editingFinished` of it.
    void ClickingARowOfTheListWritesOnePatch() {
        auto window = OpenWindow();
        QVERIFY(window);
        QQuickItem* box = window->Find("terminalFamilyBox");
        QVERIFY(box != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(box->width() > 0 && box->height() > 0,
                                 TIMEOUT_MS);
        const QString family = InstalledMonospaceFamilies().at(0);
        auto other = Listen();
        QVERIFY(other);
        QSignalSpy changed(&other->Store(), &api::StoreService::Changed);

        QCOMPARE(
            testing::OpenedRowTexts(window->Window(), box, 2, TIMEOUT_MS, true)
                .size(),
            2);
        QVERIFY(testing::ClickOpenedRow(window->Window(), box, 1));
        // This Qt says only `activated` of a click. A box that also says
        // `editingFinished` before roostd has answered must not write twice.
        QVERIFY(QMetaObject::invokeMethod(
            box->property("contentItem").value<QQuickItem*>(),
            "editingFinished"));
        QTRY_COMPARE_WITH_TIMEOUT(Document("settings").value("terminalFont"),
                                  QJsonValue(QJsonObject{{"family", family}}),
                                  TIMEOUT_MS);
        // The focus leaves the box: the same value is not written again.
        QCOMPARE(WritesBeforeBarrier(*window, changed, 14), 1);
        QTRY_COMPARE_WITH_TIMEOUT(box->property("currentText"),
                                  QVariant(family), TIMEOUT_MS);
    }

    // A name typed and Enter: one patch, also when the name is a row of the
    // list (the box then says `activated` and `editingFinished`).
    void TypingANameAndEnterWritesOnePatch_data() {
        QTest::addColumn<bool>("row");
        QTest::newRow("a name") << false;
        QTest::newRow("a row of the list") << true;
    }

    void TypingANameAndEnterWritesOnePatch() {
        QFETCH(bool, row);
        auto window = OpenWindow();
        QVERIFY(window);
        const QString family = row ? InstalledMonospaceFamilies().at(0)
                                   : QStringLiteral("Roost No Such Family");
        auto other = Listen();
        QVERIFY(other);
        QSignalSpy changed(&other->Store(), &api::StoreService::Changed);
        QQuickItem* box = FocusFamilyText(*window);
        QVERIFY(box != nullptr);

        TypeText(window->Window(), family);
        QTest::keyClick(window->Window(), Qt::Key_Return);
        // Both signals of the box for one Enter, if it says both.
        QVERIFY(QMetaObject::invokeMethod(
            box->property("contentItem").value<QQuickItem*>(),
            "editingFinished"));
        QTRY_COMPARE_WITH_TIMEOUT(Document("settings").value("terminalFont"),
                                  QJsonValue(QJsonObject{{"family", family}}),
                                  TIMEOUT_MS);
        QCOMPARE(WritesBeforeBarrier(*window, changed, 14), 1);
    }

    // A name typed and Tab (the focus leaves the box): one patch.
    void TypingANameAndTabWritesOnePatch() {
        auto window = OpenWindow();
        QVERIFY(window);
        const QString family = QStringLiteral("Roost No Such Family");
        auto other = Listen();
        QVERIFY(other);
        QSignalSpy changed(&other->Store(), &api::StoreService::Changed);
        QQuickItem* box = FocusFamilyText(*window);
        QVERIFY(box != nullptr);
        auto* input = box->property("contentItem").value<QQuickItem*>();

        TypeText(window->Window(), family);
        QTest::keyClick(window->Window(), Qt::Key_Tab);
        QTRY_VERIFY_WITH_TIMEOUT(!input->hasActiveFocus(), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Document("settings").value("terminalFont"),
                                  QJsonValue(QJsonObject{{"family", family}}),
                                  TIMEOUT_MS);
        QCOMPARE(WritesBeforeBarrier(*window, changed, 14), 1);
    }

    // Escape in the text: the text goes back to the shown value and nothing is
    // written, also when the box is left after it.
    void EscapeInTheFamilyTextWritesNothingAndRestoresTheValue() {
        auto window = OpenWindow();
        QVERIFY(window);
        const Theme* theme = window->Singleton->theme();
        auto other = Listen();
        QVERIFY(other);
        QSignalSpy changed(&other->Store(), &api::StoreService::Changed);
        QQuickItem* box = FocusFamilyText(*window);
        QVERIFY(box != nullptr);
        auto* input = box->property("contentItem").value<QQuickItem*>();

        TypeText(window->Window(), "Roost No Such Family");
        QCOMPARE(input->property("text"), QVariant("Roost No Such Family"));
        QTest::keyClick(window->Window(), Qt::Key_Escape);
        QTRY_COMPARE_WITH_TIMEOUT(input->property("text"),
                                  QVariant(DefaultFamilyEntry(*theme)),
                                  TIMEOUT_MS);
        QTest::keyClick(window->Window(), Qt::Key_Tab);
        QTRY_VERIFY_WITH_TIMEOUT(!input->hasActiveFocus(), TIMEOUT_MS);
        QCOMPARE(WritesBeforeBarrier(*window, changed, 14), 0);
        QVERIFY(!Document("settings").contains("terminalFont"));
        QCOMPARE(box->property("currentText"),
                 QVariant(DefaultFamilyEntry(*theme)));
    }

    // A document that changes while the list is open does not touch the
    // list: it stays open with its rows. When it closes the box shows the new
    // value.
    void TheDocumentChangesUnderAnOpenListAndTheBoxShowsItAfterwards() {
        auto window = OpenWindow();
        QVERIFY(window);
        QQuickItem* box = window->Find("terminalFamilyBox");
        QVERIFY(box != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(box->width() > 0 && box->height() > 0,
                                 TIMEOUT_MS);
        const QStringList installed = InstalledMonospaceFamilies();
        const int count = static_cast<int>(1 + installed.size());
        const QStringList rows = testing::OpenedRowTexts(
            window->Window(), box, std::min(count, 2), TIMEOUT_MS, true);
        QVERIFY(!rows.isEmpty());
        const QStringList model = box->property("model").toStringList();
        auto* popup = box->property("popup").value<QObject*>();
        QVERIFY(popup != nullptr);

        // Another program writes a family that is not in the list.
        const QString missing = "Roost No Such Family";
        QVERIFY(PatchSettings(
            *window, {{"terminalFont", QJsonObject{{"family", missing}}}}));
        QTRY_COMPARE_WITH_TIMEOUT(
            window->Document().value("terminalFont").toObject(),
            (QJsonObject{{"family", missing}}), TIMEOUT_MS);
        // The document has come: the list is open, its rows are the same.
        QVERIFY(popup->property("visible").toBool());
        QCOMPARE(box->property("model").toStringList(), model);
        QCOMPARE(box->property("count"), QVariant(count));

        QTest::keyClick(window->Window(), Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!popup->property("visible").toBool(),
                                 TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(box->property("currentText"),
                                  QVariant(missing + " (not installed)"),
                                  TIMEOUT_MS);
    }

    // A family of only spaces is no family: the default entry, not a missing
    // name.
    void AFamilyOfSpacesIsTheDefaultEntry() {
        auto window = OpenWindow();
        QVERIFY(window);
        const Theme* theme = window->Singleton->theme();
        QQuickItem* box = window->Find("terminalFamilyBox");
        QVERIFY(box != nullptr);
        QVERIFY(PatchSettings(
            *window, {{"terminalFont", QJsonObject{{"family", "   "}}}}));
        QTRY_COMPARE_WITH_TIMEOUT(
            window->Document().value("terminalFont").toObject(),
            (QJsonObject{{"family", "   "}}), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(box->property("currentText"),
                                  QVariant(DefaultFamilyEntry(*theme)),
                                  TIMEOUT_MS);
        QCOMPARE(box->property("currentIndex"), QVariant(0));
        QCOMPARE(box->property("count"), QVariant(1));
    }

    // The two presets of the focus ring show their colour, taken from the
    // theme; Custom has its field instead.
    void TheFocusPresetsShowTheirColour() {
        auto window = OpenWindow();
        QVERIFY(window);
        const Theme* theme = window->Singleton->theme();
        QQuickItem* colour = window->Find("focusPresetColor");
        QQuickItem* box = window->Find("focusPresetBox");
        QQuickItem* reset = window->Find("focusColorReset");
        QVERIFY(colour != nullptr && box != nullptr && reset != nullptr);
        QVERIFY(colour->isVisible());
        QCOMPARE(colour->property("text"), QVariant(theme->focusBaseText()));
        QCOMPARE(theme->focusBaseText(), QString("#225fc2"));
        QVERIFY(!reset->isVisible());

        QVERIFY(QMetaObject::invokeMethod(box, "activated", Q_ARG(int, 1)));
        QTRY_COMPARE_WITH_TIMEOUT(colour->property("text"), QVariant("#1b2430"),
                                  TIMEOUT_MS);
        QCOMPARE(theme->focusContrastText(), QString("#1b2430"));
        // The ring has the colour, and Base is still the theme's own.
        QCOMPARE(theme->focus().name(), QString("#1b2430"));
        QCOMPARE(theme->focusBaseText(), QString("#225fc2"));
        QTRY_VERIFY_WITH_TIMEOUT(reset->isVisible(), TIMEOUT_MS);

        QVERIFY(QMetaObject::invokeMethod(box, "activated", Q_ARG(int, 2)));
        QTRY_VERIFY_WITH_TIMEOUT(!colour->isVisible(), TIMEOUT_MS);
        Click(*window, reset);
        QTRY_VERIFY_WITH_TIMEOUT(!Document("settings").contains("focusColor"),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(colour->isVisible(), TIMEOUT_MS);
        QCOMPARE(colour->property("text"), QVariant("#225fc2"));
    }

    // The placeholder of an emptied field is the default, not the stored
    // value that is in force until the emptied field is left.
    void TheHintIsTheDefaultWhileTheKeyIsStored() {
        auto window = OpenWindow();
        QVERIFY(window);
        const Theme* theme = window->Singleton->theme();
        QQuickItem* terminal = window->Find("terminalSizeField");
        QQuickItem* ui = window->Find("uiSizeField");
        QQuickItem* family = window->Find("terminalFamilyBox");
        QVERIFY(terminal != nullptr && ui != nullptr && family != nullptr);
        // Sizes and a family that no system has by default.
        QVERIFY(window->Edit("terminalSizeField", "27.5"));
        QVERIFY(window->Edit("uiSizeField", "23.5"));
        QVERIFY(window->TypeFamily("Courier"));
        QTRY_COMPARE_WITH_TIMEOUT(theme->terminalFont().pointSizeF(), 27.5,
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(theme->uiFont().pointSizeF(), 23.5,
                                  TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            window->Document().value("terminalFont").toObject().size(), 2,
            TIMEOUT_MS);
        // The fields are emptied, not yet left: the keys are still stored.
        QVERIFY(terminal->setProperty("text", ""));
        QVERIFY(ui->setProperty("text", ""));
        QCOMPARE(terminal->property("placeholderText"),
                 QVariant("default: " +
                          Theme::sizeText(theme->defaultTerminalFont())));
        QCOMPARE(
            ui->property("placeholderText"),
            QVariant("default: " + Theme::sizeText(theme->defaultUiFont())));
        // The first entry of the family box is the default, not the stored
        // name.
        QCOMPARE(family->property("model").toStringList().first(),
                 DefaultFamilyEntry(*theme));
        QVERIFY(theme->defaultTerminalFont().pointSizeF() != 27.5);
        QVERIFY(theme->defaultUiFont().pointSizeF() != 23.5);
    }

    // Reset of the family removes the family and leaves the size.
    void ResetOfTheFamilyKeepsTheSize() {
        auto window = OpenWindow();
        QVERIFY(window);
        QVERIFY(window->Edit("terminalSizeField", "15"));
        QVERIFY(window->TypeFamily("Courier"));
        QTRY_COMPARE_WITH_TIMEOUT(
            Document("settings").value("terminalFont").toObject(),
            (QJsonObject{{"family", "Courier"}, {"size", 15}}), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(
            window->Document().value("terminalFont").toObject().size(), 2,
            TIMEOUT_MS);
        QQuickItem* reset = window->Find("terminalFamilyReset");
        QVERIFY(reset != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(reset->isVisible(), TIMEOUT_MS);
        Click(*window, reset);
        QTRY_COMPARE_WITH_TIMEOUT(
            Document("settings").value("terminalFont").toObject(),
            (QJsonObject{{"size", 15}}), TIMEOUT_MS);
        // The file is written before the window hears of it: wait for the
        // field, not only for the file.
        QTRY_COMPARE_WITH_TIMEOUT(
            window->Find("terminalFamilyBox")->property("currentIndex"),
            QVariant(0), TIMEOUT_MS);
    }

    // A terminalFont that is no object is stored: both rows of the font
    // have Reset, and it removes the whole key.
    void AFontThatIsNoObjectCanBeReset_data() {
        QTest::addColumn<QString>("reset");
        QTest::newRow("family") << "terminalFamilyReset";
        QTest::newRow("size") << "terminalSizeReset";
    }

    void AFontThatIsNoObjectCanBeReset() {
        QFETCH(QString, reset);
        auto window = OpenWindow();
        QVERIFY(window);
        QQuickItem* family = window->Find("terminalFamilyReset");
        QQuickItem* size = window->Find("terminalSizeReset");
        QVERIFY(family != nullptr && size != nullptr);
        QVERIFY(!family->isVisible() && !size->isVisible());
        QVERIFY(PatchSettings(*window, {{"terminalFont", "big"}}));
        QTRY_VERIFY_WITH_TIMEOUT(family->isVisible() && size->isVisible(),
                                 TIMEOUT_MS);
        Click(*window, window->Find(reset));
        QTRY_VERIFY_WITH_TIMEOUT(!Document("settings").contains("terminalFont"),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!family->isVisible() && !size->isVisible(),
                                 TIMEOUT_MS);
    }

    // A focusColor that is stored in any form has Reset: a wrong type, and an
    // object that has a colour but no preset.
    void AFocusColorInAnyFormCanBeReset_data() {
        QTest::addColumn<QJsonValue>("stored");
        QTest::newRow("not an object") << QJsonValue("red");
        QTest::newRow("a colour without a preset")
            << QJsonValue(QJsonObject{{"custom", "#112233"}});
    }

    void AFocusColorInAnyFormCanBeReset() {
        QFETCH(QJsonValue, stored);
        auto window = OpenWindow();
        QVERIFY(window);
        QQuickItem* reset = window->Find("focusColorReset");
        QVERIFY(reset != nullptr);
        QVERIFY(!reset->isVisible());
        QVERIFY(PatchSettings(*window, {{"focusColor", stored}}));
        QTRY_VERIFY_WITH_TIMEOUT(reset->isVisible(), TIMEOUT_MS);
        Click(*window, reset);
        QTRY_VERIFY_WITH_TIMEOUT(!Document("settings").contains("focusColor"),
                                 TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(!reset->isVisible(), TIMEOUT_MS);
    }

    // A stored value that is not applied: the font in force is named under
    // the field, and the line goes when the value is put right.
    void TheFontInUseIsSaidWhileAStoredValueIsNotApplied() {
        auto window = OpenWindow();
        QVERIFY(window);
        const Theme* theme = window->Singleton->theme();
        QQuickItem* terminal = window->Find("terminalFontInUse");
        QQuickItem* ui = window->Find("uiFontInUse");
        QVERIFY(terminal != nullptr && ui != nullptr);
        QVERIFY(!terminal->isVisible() && !ui->isVisible());

        // A family that is not installed: the system font is in use.
        const QString missing = "Roost No Such Family";
        QVERIFY(window->TypeFamily(missing));
        QTRY_VERIFY_WITH_TIMEOUT(terminal->isVisible(), TIMEOUT_MS);
        QCOMPARE(terminal->property("text"),
                 QVariant(InUseText(theme->terminalFont())));
        QVERIFY(theme->terminalFont().family() != missing);
        QVERIFY(terminal->property("text").toString().contains(
            theme->defaultTerminalFont().family()));
        QVERIFY(window->TypeFamily(""));
        QTRY_VERIFY_WITH_TIMEOUT(!terminal->isVisible(), TIMEOUT_MS);

        // A size out of range: the default size is in use.
        QVERIFY(window->Edit("terminalSizeField", "500"));
        QTRY_VERIFY_WITH_TIMEOUT(terminal->isVisible(), TIMEOUT_MS);
        QCOMPARE(terminal->property("text"),
                 QVariant(InUseText(theme->terminalFont())));
        QCOMPARE(theme->terminalFont().pointSizeF(),
                 theme->defaultTerminalFont().pointSizeF());
        QVERIFY(window->Edit("terminalSizeField", "15"));
        QTRY_VERIFY_WITH_TIMEOUT(!terminal->isVisible(), TIMEOUT_MS);

        QVERIFY(window->Edit("uiSizeField", "500"));
        QTRY_VERIFY_WITH_TIMEOUT(ui->isVisible(), TIMEOUT_MS);
        QCOMPARE(ui->property("text"), QVariant(InUseText(theme->uiFont())));
        QCOMPARE(theme->uiFont().pointSizeF(),
                 theme->defaultUiFont().pointSizeF());
        QVERIFY(window->Edit("uiSizeField", "14"));
        QTRY_VERIFY_WITH_TIMEOUT(!ui->isVisible(), TIMEOUT_MS);
        QCOMPARE(ui->property("text"), QVariant(InUseText(theme->uiFont())));
    }
};

QTEST_MAIN(SettingsWindowTest)
#include "settings_window_test.moc"
