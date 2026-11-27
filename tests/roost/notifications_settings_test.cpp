// The settings page of the notifications plugin (plugins/notifications/qml/
// NotificationsSettings.qml, the category "Notifications" of the settings
// window): the real roostd with the real plugin (its page and its backend), the
// real window and its page, key presses and clicks. What the page writes is
// read back from the document of the plugin through the socket, and the
// commands of the user are small bash lines that write into the directory of
// the test. The real settings of the user are never read: ROOST_CONFIG is the
// directory of roostd's own temporary tree.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <functional>
#include <memory>

#include "api/api.hpp"
#include "api/api_test_support.hpp"
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

const QString PLUGIN = QStringLiteral("notifications");
const QString CATEGORY = QStringLiteral("plugin:notifications");

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

QList<QQuickItem*> FindAll(QQuickItem* root, const QString& name) {
    QList<QQuickItem*> found;
    FindItems(root, name, found);
    return found;
}

QJsonArray Events(const QStringList& names) {
    QJsonArray events;
    for (const QString& name : names) {
        events.append(name);
    }
    return events;
}

}  // namespace

// One window of roost: its own Api (a connection to roostd), engine, loader and
// the settings window with its pages.
struct NotificationsWindow {
    std::unique_ptr<api::Api> Api = std::make_unique<api::Api>();
    std::unique_ptr<QQmlEngine> Engine = std::make_unique<QQmlEngine>();
    RoostSingleton* Singleton = nullptr;
    std::unique_ptr<PluginLoader> Loader;
    std::unique_ptr<QObject> Object;
    std::unique_ptr<SettingsPages> Pages;

    ~NotificationsWindow() {
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
        return ::FindAll(Window()->contentItem(), name);
    }
};

class NotificationsSettingsTest : public QObject {
    Q_OBJECT

private:
    // A window against roostd, on the category of the plugin; null if it could
    // not be made.
    std::unique_ptr<NotificationsWindow> OpenWindow() {
        auto window = std::make_unique<NotificationsWindow>();
        QSignalSpy connected(window->Api.get(), &api::Api::Connected);
        window->Singleton = window->Engine->singletonInstance<RoostSingleton*>(
            "Roost.Api", "Roost");
        if (window->Singleton == nullptr) {
            return nullptr;
        }
        window->Singleton->setApi(*window->Api);
        window->Api->Connect(Daemon_->SocketPath());
        if (connected.isEmpty() && !connected.wait(TIMEOUT_MS)) {
            return nullptr;
        }
        if (!QTest::qWaitFor([&] { return window->Api->Plugins().IsLoaded(); },
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
        const QVariant target = QVariantMap{{"plugin", PLUGIN}};
        if (!QMetaObject::invokeMethod(window->Object.get(), "open",
                                       Q_ARG(QVariant, target))) {
            return nullptr;
        }
        return window;
    }

    static QStringList CategoryTitles(const NotificationsWindow& window) {
        QList<QQuickItem*> rows = window.FindAll("categoryItem");
        std::ranges::sort(rows, [](const QQuickItem* a, const QQuickItem* b) {
            return a->y() < b->y();
        });
        QStringList titles;
        for (const QQuickItem* row : rows) {
            titles.append(row->property("text").toString());
        }
        return titles;
    }

    // The page is there, shown and laid out.
    static bool PageIsReady(const NotificationsWindow& window) {
        const QQuickItem* page = window.Find("pluginSettings");
        const QQuickItem* add = window.Find("addCommand");
        return page != nullptr && page->isVisible() && add != nullptr &&
               add->width() > 0 && add->height() > 0;
    }

    static QQuickItem* Focused(const NotificationsWindow& window) {
        return window.Window()->activeFocusItem();
    }

    static QString FocusedName(const NotificationsWindow& window) {
        const QQuickItem* focused = Focused(window);
        return focused == nullptr ? QString() : focused->objectName();
    }

    // The rows of the commands, top to bottom.
    static QList<QQuickItem*> Rows(const NotificationsWindow& window) {
        QList<QQuickItem*> rows = window.FindAll("commandRow");
        std::ranges::sort(rows, [](const QQuickItem* a, const QQuickItem* b) {
            return a->mapToScene(QPointF()).y() < b->mapToScene(QPointF()).y();
        });
        return rows;
    }

    // The control of the name in the row at `index`, null for none.
    static QQuickItem* InRow(const NotificationsWindow& window, int index,
                             const QString& name) {
        const QList<QQuickItem*> rows = Rows(window);
        return index < rows.size() ? FindItem(rows.at(index), name) : nullptr;
    }

    // What a user does to a control: reaches it (Tab, which scrolls the page to
    // it) and clicks its middle. `find` is called again after each wait: a
    // document that arrives makes rows anew.
    static void Click(const NotificationsWindow& window,
                      const std::function<QQuickItem*()>& find) {
        QTRY_VERIFY_WITH_TIMEOUT(
            find() != nullptr && find()->isVisible() && find()->isEnabled() &&
                find()->width() > 0 && find()->height() > 0,
            TIMEOUT_MS);
        // A document that has just arrived moves its rows at the polish of the
        // next frame: a click before it lands where the control was.
        QSignalSpy frame(window.Window(), &QQuickWindow::frameSwapped);
        window.Window()->update();
        QVERIFY(frame.wait(TIMEOUT_MS));
        QQuickItem* item = find();
        QVERIFY(item != nullptr);
        item->forceActiveFocus(Qt::TabFocusReason);
        QQuickItem* scroll = window.Find("settingsScroll");
        QVERIFY(scroll != nullptr);
        const auto middle = [&] {
            QQuickItem* now = find();
            return now == nullptr ? QPointF(-1, -1)
                                  : now->mapToScene(QPointF(now->width() / 2,
                                                            now->height() / 2));
        };
        QTRY_VERIFY_WITH_TIMEOUT(
            scroll
                ->mapRectToScene(
                    QRectF(0, 0, scroll->width(), scroll->height()))
                .contains(middle()),
            TIMEOUT_MS);
        QTest::mouseClick(window.Window(), Qt::LeftButton, Qt::NoModifier,
                          middle().toPoint());
    }

    static void ClickNamed(const NotificationsWindow& window,
                           const QString& name) {
        Click(window, [&] { return window.Find(name); });
    }

    static void ClickInRow(const NotificationsWindow& window, int index,
                           const QString& name) {
        Click(window, [&] { return InRow(window, index, name); });
    }

    static void TypeText(QQuickWindow* window, const QString& text) {
        for (const QChar ch : text) {
            QTest::keyClick(window, ch.toLatin1());
        }
    }

    // The document of the plugin as roostd has it, read through the socket.
    QJsonObject Document() const {
        const QFuture<rpc::Result> read = Reader_->Store().Get(PLUGIN);
        if (!WaitFor(read) || !read.result()) {
            return {};
        }
        return read.result()->toObject().value("document").toObject();
    }

    QJsonObject Settings() const {
        return Document().value("settings").toObject();
    }

    // What another program, or a hand edit, writes to the document.
    bool Patch(const QJsonObject& patch) const {
        const QFuture<rpc::Result> writing =
            Reader_->Store().Patch(PLUGIN, patch);
        return WaitFor(writing) && writing.result();
    }

    bool PutCommands(const QJsonArray& commands) const {
        return Patch({{"settings", QJsonObject{{"commands", commands}}}});
    }

    QString Path(const QString& name) const { return Work_->filePath(name); }

    // The text of a command of the user that writes a file of the test.
    QString WriteFile(const QString& name) const {
        return QStringLiteral("echo written > '%1'").arg(Path(name));
    }

    // The page has shown `count` commands.
    static bool ShowsRows(const NotificationsWindow& window, int count) {
        const QList<QQuickItem*> rows = Rows(window);
        return rows.size() == count &&
               std::ranges::all_of(rows, [](const QQuickItem* row) {
                   return row->width() > 0 && row->height() > 0;
               });
    }

    static QString TextIn(const NotificationsWindow& window, int index,
                          const QString& name) {
        const QQuickItem* item = InRow(window, index, name);
        return item == nullptr ? QString() : item->property("text").toString();
    }

    // The window on the page, the commands that the user has already.
    std::unique_ptr<NotificationsWindow> OpenWithCommands(
        const QJsonArray& commands) {
        if (!commands.isEmpty() && !PutCommands(commands)) {
            return nullptr;
        }
        auto window = OpenWindow();
        if (window == nullptr ||
            !QTest::qWaitFor([&] { return PageIsReady(*window); },
                             TIMEOUT_MS) ||
            !QTest::qWaitFor(
                [&] {
                    return ShowsRows(*window,
                                     static_cast<int>(commands.size()));
                },
                TIMEOUT_MS)) {
            return nullptr;
        }
        return window;
    }

private slots:
    void initTestCase() { roost::SelectControlsStyle(); }

    void init() {
        Work_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Work_->isValid());
        Daemon_ = std::make_unique<testing::RoostdProcess>();
        QVERIFY(Daemon_->IsValid());
        Config_ = std::make_unique<testing::ScopedEnv>(
            "ROOST_CONFIG", Daemon_->ConfigPath().toUtf8());
        QVERIFY(QDir().mkpath(Daemon_->PluginsPath()));
        QVERIFY(QFile::link(NOTIFICATIONS_PLUGIN_DIR,
                            Daemon_->PluginsPath() + "/notifications"));
        QVERIFY(Daemon_->Start(ROOSTD_PATH));
        Reader_ = std::make_unique<api::Api>();
        QSignalSpy connected(Reader_.get(), &api::Api::Connected);
        Reader_->Connect(Daemon_->SocketPath());
        QVERIFY(connected.wait(TIMEOUT_MS) || !connected.isEmpty());
        // The backend has started and read its document when it answers a
        // command: Test is then not refused for a backend that is not there.
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                const QFuture<rpc::Result> ran = Reader_->Commands().Run(
                    "notifications.test", QJsonObject{});
                return WaitFor(ran) && ran.result();
            }(),
            TIMEOUT_MS);
    }

    void cleanup() {
        Reader_.reset();
        Daemon_.reset();
        Config_.reset();
        Work_.reset();
    }

    // The category is in the list, after the built-in ones, and the page shows
    // what the backend takes for an empty document.
    void TheCategoryShowsTheDefaultsOfAnEmptyDocument() {
        auto window = OpenWindow();
        QVERIFY(window);
        QTRY_COMPARE_WITH_TIMEOUT(
            CategoryTitles(*window),
            (QStringList{"Appearance", "Keys", "Updates", "Notifications"}),
            TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(PageIsReady(*window), TIMEOUT_MS);
        QCOMPARE(window->Object->property("shownKey").toString(), CATEGORY);
        QTRY_VERIFY_WITH_TIMEOUT(
            window->Find("desktopSwitch")->property("checked").toBool(),
            TIMEOUT_MS);
        QVERIFY(window->Find("blockedSwitch")->property("checked").toBool());
        QVERIFY(!window->Find("doneSwitch")->property("checked").toBool());
        QVERIFY(window->Find("failedSwitch")->property("checked").toBool());
        QVERIFY(Rows(*window).isEmpty());
        // Nothing is written by looking.
        QVERIFY(Document().isEmpty());
    }

    // The switches write the key that was changed, and keep the rest.
    void TheSwitchesWriteTheDocument() {
        auto window = OpenWindow();
        QVERIFY(window);
        QTRY_VERIFY_WITH_TIMEOUT(PageIsReady(*window), TIMEOUT_MS);
        ClickNamed(*window, "desktopSwitch");
        QTRY_COMPARE_WITH_TIMEOUT(Settings().value("desktop"),
                                  QJsonValue(false), TIMEOUT_MS);
        ClickNamed(*window, "doneSwitch");
        QTRY_COMPARE_WITH_TIMEOUT(Settings().value("events").toArray(),
                                  Events({"blocked", "done", "failed"}),
                                  TIMEOUT_MS);
        // The key on the switch that has the keyboard toggles it too.
        window->Find("blockedSwitch")->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window->Window(), Qt::Key_Space);
        QTRY_COMPARE_WITH_TIMEOUT(Settings().value("events").toArray(),
                                  Events({"done", "failed"}), TIMEOUT_MS);
        QCOMPARE(Settings().value("desktop"), QJsonValue(false));
    }

    // Add command makes a row with a free name and the keyboard on its name;
    // the name and the command are typed, Enter writes each; Remove takes the
    // command out.
    void AddACommandTypeItAndRemoveIt() {
        auto window = OpenWindow();
        QVERIFY(window);
        QTRY_VERIFY_WITH_TIMEOUT(PageIsReady(*window), TIMEOUT_MS);
        ClickNamed(*window, "addCommand");
        QTRY_VERIFY_WITH_TIMEOUT(ShowsRows(*window, 1), TIMEOUT_MS);
        QCOMPARE(TextIn(*window, 0, "commandName"), QString("command 1"));
        QTRY_COMPARE_WITH_TIMEOUT(Focused(*window),
                                  InRow(*window, 0, "commandName"), TIMEOUT_MS);
        // A second one is "command 2".
        ClickNamed(*window, "addCommand");
        QTRY_VERIFY_WITH_TIMEOUT(ShowsRows(*window, 2), TIMEOUT_MS);
        QCOMPARE(TextIn(*window, 1, "commandName"), QString("command 2"));
        ClickInRow(*window, 1, "commandRemove");
        QTRY_VERIFY_WITH_TIMEOUT(ShowsRows(*window, 1), TIMEOUT_MS);

        // The name has the keyboard and is all selected: typing replaces it.
        InRow(*window, 0, "commandName")->forceActiveFocus(Qt::TabFocusReason);
        QMetaObject::invokeMethod(InRow(*window, 0, "commandName"),
                                  "selectAll");
        TypeText(window->Window(), "phone");
        QTest::keyClick(window->Window(), Qt::Key_Return);
        QTest::keyClick(window->Window(), Qt::Key_Tab);
        QCOMPARE(FocusedName(*window), QString("commandRun"));
        TypeText(window->Window(), "echo hi");
        QTest::keyClick(window->Window(), Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(
            Settings().value("commands").toArray(),
            (QJsonArray{
                QJsonObject{{"name", "phone"},
                            {"run", "echo hi"},
                            {"events", Events({"blocked", "done", "failed"})},
                            {"enabled", true}}}),
            TIMEOUT_MS);

        ClickInRow(*window, 0, "commandRemove");
        QTRY_VERIFY_WITH_TIMEOUT(ShowsRows(*window, 0), TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Settings().value("commands").toArray(),
                                  QJsonArray{}, TIMEOUT_MS);
        // The keyboard is not lost with the row.
        QCOMPARE(FocusedName(*window), QString("addCommand"));
    }

    // The checkboxes of the events and the switch of a command are written with
    // the whole list; a command that stores no events shows all three.
    void TheEventsAndTheSwitchOfACommandAreWritten() {
        auto window =
            OpenWithCommands({QJsonObject{{"name", "all"}, {"run", "true"}},
                              QJsonObject{{"name", "one"},
                                          {"run", "true"},
                                          {"events", Events({"done"})},
                                          {"enabled", false}}});
        QVERIFY(window);
        for (const QString& name : {"commandBlocked", "commandDone",
                                    "commandFailed", "commandEnabled"}) {
            QVERIFY(InRow(*window, 0, name)->property("checked").toBool());
        }
        QVERIFY(
            !InRow(*window, 1, "commandBlocked")->property("checked").toBool());
        QVERIFY(InRow(*window, 1, "commandDone")->property("checked").toBool());
        QVERIFY(
            !InRow(*window, 1, "commandEnabled")->property("checked").toBool());
        ClickInRow(*window, 0, "commandFailed");
        ClickInRow(*window, 1, "commandEnabled");
        QTRY_COMPARE_WITH_TIMEOUT(
            Settings().value("commands").toArray(),
            (QJsonArray{QJsonObject{{"name", "all"},
                                    {"run", "true"},
                                    {"events", Events({"blocked", "done"})},
                                    {"enabled", true}},
                        QJsonObject{{"name", "one"},
                                    {"run", "true"},
                                    {"events", Events({"done"})},
                                    {"enabled", true}}}),
            TIMEOUT_MS);
    }

    // Test runs the command and the row shows how it went: the exit code, and
    // the stderr of a command that failed.
    void TestRunsTheCommandAndShowsTheResult() {
        auto window = OpenWithCommands(
            {QJsonObject{{"name", "touch"}, {"run", WriteFile("made")}},
             QJsonObject{{"name", "fail"}, {"run", "echo oops >&2; exit 3"}}});
        QVERIFY(window);
        QVERIFY(TextIn(*window, 0, "commandResult").isEmpty());

        ClickInRow(*window, 0, "commandTest");
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(Path("made")), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(
            QRegularExpression(QStringLiteral(R"(^exit 0 · \d\d:\d\d$)"))
                .match(TextIn(*window, 0, "commandResult"))
                .hasMatch(),
            TIMEOUT_MS);
        // The button is back for another run.
        QTRY_VERIFY_WITH_TIMEOUT(InRow(*window, 0, "commandTest")->isEnabled(),
                                 TIMEOUT_MS);

        ClickInRow(*window, 1, "commandTest");
        QTRY_VERIFY_WITH_TIMEOUT(
            QRegularExpression(QStringLiteral(R"(^exit 3 · \d\d:\d\d$)"))
                .match(TextIn(*window, 1, "commandResult"))
                .hasMatch(),
            TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(TextIn(*window, 1, "commandStderr"),
                                  QString("oops"), TIMEOUT_MS);
        // The other row has its own result.
        QVERIFY(TextIn(*window, 0, "commandResult").startsWith("exit 0"));
    }

    // A command that a signal killed shows how it ended: no exit code.
    void ACommandKilledBySignalShowsAsCrashed() {
        auto window = OpenWithCommands(
            {QJsonObject{{"name", "broken"}, {"run", "kill -9 $$"}}});
        QVERIFY(window);
        ClickInRow(*window, 0, "commandTest");
        QTRY_VERIFY_WITH_TIMEOUT(
            QRegularExpression(QStringLiteral(R"(^crashed · \d\d:\d\d$)"))
                .match(TextIn(*window, 0, "commandResult"))
                .hasMatch(),
            TIMEOUT_MS);
    }

    // A name that another command has, or an empty one, is refused with the
    // reason; the old name stays and nothing is written.
    void ADuplicateOrEmptyNameIsRefused() {
        const QJsonArray commands{
            QJsonObject{{"name", "first"}, {"run", "true"}},
            QJsonObject{{"name", "second"}, {"run", "true"}}};
        auto window = OpenWithCommands(commands);
        QVERIFY(window);
        const QJsonArray before = Settings().value("commands").toArray();

        QQuickItem* name = InRow(*window, 1, "commandName");
        name->forceActiveFocus(Qt::TabFocusReason);
        QMetaObject::invokeMethod(name, "selectAll");
        TypeText(window->Window(), "first");
        QTest::keyClick(window->Window(), Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(
            TextIn(*window, 1, "commandProblem"),
            QString("A command named \"first\" exists already."), TIMEOUT_MS);
        QCOMPARE(TextIn(*window, 1, "commandName"), QString("second"));

        QMetaObject::invokeMethod(name, "selectAll");
        QTest::keyClick(window->Window(), Qt::Key_Delete);
        QTest::keyClick(window->Window(), Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(TextIn(*window, 1, "commandProblem"),
                                  QString("The name is empty."), TIMEOUT_MS);
        QCOMPARE(TextIn(*window, 1, "commandName"), QString("second"));

        // The barrier: the write of a switch comes after whatever the refused
        // edits would have written.
        ClickNamed(*window, "desktopSwitch");
        QTRY_COMPARE_WITH_TIMEOUT(Settings().value("desktop"),
                                  QJsonValue(false), TIMEOUT_MS);
        QCOMPARE(Settings().value("commands").toArray(), before);
        // A good name takes the message away.
        name->forceActiveFocus(Qt::TabFocusReason);
        QMetaObject::invokeMethod(name, "selectAll");
        TypeText(window->Window(), "third");
        QTest::keyClick(window->Window(), Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(TextIn(*window, 1, "commandProblem").isEmpty(),
                                 TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(Settings()
                                      .value("commands")
                                      .toArray()
                                      .at(1)
                                      .toObject()
                                      .value("name"),
                                  QJsonValue("third"), TIMEOUT_MS);
    }

    // A second writer changes the document while a field is being edited: the
    // page takes the change and the typed text stays.
    void AnotherWriterDoesNotTakeTheTextOfAFieldBeingEdited() {
        auto window =
            OpenWithCommands({QJsonObject{{"name", "one"}, {"run", "true"}},
                              QJsonObject{{"name", "two"}, {"run", "true"}}});
        QVERIFY(window);
        QQuickItem* run = InRow(*window, 0, "commandRun");
        run->forceActiveFocus(Qt::TabFocusReason);
        QMetaObject::invokeMethod(run, "selectAll");
        TypeText(window->Window(), "mine");

        QVERIFY(PutCommands({QJsonObject{{"name", "one"}, {"run", "theirs"}},
                             QJsonObject{{"name", "two"}, {"run", "theirs"}}}));
        QTRY_COMPARE_WITH_TIMEOUT(TextIn(*window, 1, "commandRun"),
                                  QString("theirs"), TIMEOUT_MS);
        QCOMPARE(TextIn(*window, 0, "commandRun"), QString("mine"));
        QCOMPARE(Focused(*window), run);
    }

    // Tab goes through every control of a row, in reading order, and on to the
    // button below.
    void TabReachesTheControlsOfACommandRow() {
        auto window =
            OpenWithCommands({QJsonObject{{"name", "one"}, {"run", "true"}}});
        QVERIFY(window);
        InRow(*window, 0, "commandName")->forceActiveFocus(Qt::TabFocusReason);
        QStringList seen{FocusedName(*window)};
        for (int i = 0; i < 8; ++i) {
            QTest::keyClick(window->Window(), Qt::Key_Tab);
            seen.append(FocusedName(*window));
        }
        QCOMPARE(seen,
                 (QStringList{"commandName", "commandRun", "commandBlocked",
                              "commandDone", "commandFailed", "commandEnabled",
                              "commandTest", "commandRemove", "addCommand"}));
        // Shift+Tab goes back the same way.
        QTest::keyClick(window->Window(), Qt::Key_Tab, Qt::ShiftModifier);
        QCOMPARE(FocusedName(*window), QString("commandRemove"));
    }

    // Pictures of the page for a person to look at, with ROOST_TEST_SNAPSHOTS
    // set to a directory: in the light theme and in a dark one, a command that
    // has run and failed on show.
    void SnapshotsOfThePage() {
        auto window = OpenWithCommands(
            {QJsonObject{{"name", "phone"},
                         {"run", "echo \"$ROOST_MESSAGE\""},
                         {"events", Events({"blocked", "failed"})}},
             QJsonObject{{"name", "broken"},
                         {"run", "echo 'no such device' >&2; exit 3"}}});
        QVERIFY(window);
        ClickInRow(*window, 0, "commandTest");
        ClickInRow(*window, 1, "commandTest");
        QTRY_VERIFY_WITH_TIMEOUT(
            TextIn(*window, 0, "commandResult").startsWith("exit 0") &&
                TextIn(*window, 1, "commandResult").startsWith("exit 3"),
            TIMEOUT_MS);
        window->Window()->resize(900, 760);
        QVERIFY(testing::SaveSnapshot(window->Window(), "light"));
        QVERIFY(window->Singleton->theme()->load(DarkTheme()));
        QVERIFY(testing::SaveSnapshot(window->Window(), "dark"));
    }

private:
    // The tokens that a dark theme sets over the default one.
    static QJsonObject DarkTheme() {
        return {{"windowBackground", "#14181d"},
                {"windowText", "#e6ebf0"},
                {"controlBackground", "#1a2027"},
                {"inputBackground", "#10141a"},
                {"panelBackground", "#1b2128"},
                {"text", "#e6ebf0"},
                {"textStrong", "#ffffff"},
                {"textSecondary", "#a9b6c4"},
                {"textMuted", "#97a5b5"},
                {"selection", "#2c4a7a"},
                {"warning", "#f0b45a"},
                {"error", "#ff8a8f"},
                {"canvas", "#14181d"},
                {"surface", "#1b2128"},
                {"line", "#2f3944"},
                {"selectedBackground", "#22324d"},
                {"focus", "#7fb0ff"},
                {"primary", "#4f86e6"},
                {"primaryHover", "#6a9aee"},
                {"hoverBackground", "#232b34"},
                {"primaryText", "#0b1220"}};
    }

    std::unique_ptr<QTemporaryDir> Work_;
    std::unique_ptr<testing::RoostdProcess> Daemon_;
    std::unique_ptr<testing::ScopedEnv> Config_;
    std::unique_ptr<api::Api> Reader_;
};

QTEST_MAIN(NotificationsSettingsTest)
#include "notifications_settings_test.moc"
