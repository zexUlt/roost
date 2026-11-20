// The built-in list of sessions is replaced by a plugin of the user: a copy of
// templates/plugin-qml in the plugins directory of the real roostd, with a
// list panel `alt-sessions.list` whose output `selected` is a SessionRef, and
// `plugins.disabled: ["sessions"]` in the settings document. The window is
// the unmodified build; the whole replacement is that directory and the
// documents of the store. Selecting a row of the new list connects the
// terminal, and the panel sessions.list is not created.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlExtensionPlugin>
#include <QTest>

#include <optional>

#include "helpers/layout_window.hpp"
#include "helpers/scoped_env.hpp"

using namespace roost;
using namespace roost::testing;

Q_IMPORT_QML_PLUGIN(Roost_ApiPlugin)
Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin)

namespace {

const QString PLUGIN_ID = "alt-sessions";
const QString ALT_LIST = PLUGIN_ID + ".list";

// A list of Roost.sessions whose row, when tapped, becomes `selected`.
constexpr const char* ALT_LIST_QML = R"(
import QtQuick
import Roost.Api

Item {
    id: root

    property var selected: null
    // RESTORED
    ListView {
        objectName: "altList"
        anchors.fill: parent
        model: Roost.sessions
        delegate: Rectangle {
            id: row
            required property string agent
            required property string id
            objectName: "altRow"
            property string sessionId: id
            width: ListView.view.width
            height: 24
            Text {
                text: row.id
            }
            MouseArea {
                anchors.fill: parent
                onClicked: root.selected = {"agent": row.agent, "id": row.id}
            }
        }
    }
}
)";

bool CopyTree(const QString& from, const QString& to) {
    QDirIterator files(from, QDir::Files, QDirIterator::Subdirectories);
    const QDir source(from);
    while (files.hasNext()) {
        const QString path = files.next();
        const QString target = to + u'/' + source.relativeFilePath(path);
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) ||
            !QFile::copy(path, target)) {
            return false;
        }
    }
    return true;
}

bool WriteFile(const QString& path, const QByteArray& content) {
    QFile file(path);
    return QDir().mkpath(QFileInfo(path).absolutePath()) &&
           file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(content) == content.size();
}

}  // namespace

class PluginReplaceTest : public LayoutWindow {
    Q_OBJECT

private:
    // The template, with the id of the plugin rewritten as its README says and
    // one more panel: the list.
    // `directory` decides the load order: by name, as roostd finds them.
    // `restoring`: the list has the optional property `restored`, false until
    // the test says otherwise (--smoke-sessions waits for it).
    bool InstallFixture(const QString& directory = "fixture",
                        bool restoring = false) {
        const QString target = Dir_->filePath("plugins/" + directory);
        if (!CopyTree(QStringLiteral(PLUGIN_TEMPLATE_DIR), target)) {
            return false;
        }
        QFile manifestFile(target + "/plugin.json");
        if (!manifestFile.open(QIODevice::ReadOnly)) {
            return false;
        }
        QJsonObject manifest =
            QJsonDocument::fromJson(manifestFile.readAll()).object();
        manifestFile.close();
        // The template must have the id to be rewritten, not get one here.
        if (!manifest.contains("id")) {
            return false;
        }
        manifest["id"] = PLUGIN_ID;
        QJsonObject contributes = manifest["contributes"].toObject();
        QJsonArray panels = contributes["panels"].toArray();
        panels.append(
            QJsonObject{{"id", "list"},
                        {"qml", "qml/AltList.qml"},
                        {"inputs", QJsonObject{}},
                        {"outputs", QJsonObject{{"selected", "SessionRef"}}}});
        contributes["panels"] = panels;
        manifest["contributes"] = contributes;
        return WriteFile(
                   target + "/qml/AltList.qml",
                   QByteArray(ALT_LIST_QML)
                       .replace(
                           "// RESTORED",
                           restoring ? "property bool restored: false" : "")) &&
               WriteFile(target + "/plugin.json",
                         QJsonDocument(manifest).toJson());
    }

    bool WriteStore(const QString& name, const QJsonObject& document) {
        return WriteFile(Dir_->filePath("config/" + name + ".json"),
                         QJsonDocument(document).toJson());
    }

    // The fixture is installed, `sessions` is disabled in the settings and
    // stays in the plugins directory with the other built-in plugins.
    bool StartReplaced(const QJsonObject& layout = {}, bool restoring = false) {
        ListType_ = ALT_LIST;
        return InstallFixture("fixture", restoring) &&
               WriteStore(
                   "settings",
                   {{"plugins",
                     QJsonObject{{"disabled", QJsonArray{"sessions"}}}}}) &&
               (layout.isEmpty() || WriteStore("layout", layout)) &&
               WriteFakeClaude(FAKE_CLAUDE) && StartDaemon();
    }

    QQuickItem* ViewOf(const QString& list) {
        QQuickItem* panel = PanelByInstance(list);
        return panel == nullptr ? nullptr
                                : panel->findChild<QQuickItem*>("altList");
    }

    int RowCount(const QString& list) {
        QQuickItem* view = ViewOf(list);
        return view == nullptr ? 0 : view->property("count").toInt();
    }

    // The delegate of the session; null while the view has not made it.
    QQuickItem* RowOf(const QString& list, const QString& id) {
        QQuickItem* view = ViewOf(list);
        for (int index = 0; view != nullptr && index < RowCount(list);
             ++index) {
            QQuickItem* row = nullptr;
            QMetaObject::invokeMethod(view, "itemAtIndex",
                                      Q_RETURN_ARG(QQuickItem*, row),
                                      Q_ARG(int, index));
            if (row != nullptr && row->property("sessionId").toString() == id) {
                return row;
            }
        }
        return nullptr;
    }

    // A tap on the row of the session, through the window.
    void TapSession(const QString& list, const QString& id) {
        QTRY_VERIFY_WITH_TIMEOUT(RowOf(list, id) != nullptr, TIMEOUT_MS);
        QQuickItem* row = RowOf(list, id);
        QTest::mouseClick(
            Window(), Qt::LeftButton, Qt::NoModifier,
            row->mapToScene(QPointF(row->width() / 2, row->height() / 2))
                .toPoint());
    }

    int PanelsOfType(const QString& type) {
        int count = 0;
        for (auto* slot : Root()->findChildren<QQuickItem*>("panelSlot")) {
            count += slot->property("panelType").toString() == type &&
                     slot->property("panel").value<QQuickItem*>() != nullptr;
        }
        return count;
    }

    // What the window and roostd say of the replacement, whatever the layout.
    void VerifyReplaced(const QString& list, const QString& terminal) {
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance(list) != nullptr &&
                                     PanelByInstance(terminal) != nullptr,
                                 TIMEOUT_MS);
        QCOMPARE(SlotByInstance(list)->property("panelType").toString(),
                 ALT_LIST);
        // roostd did not load `sessions`, and says nothing is wrong.
        QStringList loaded;
        for (const QJsonValue& plugin :
             Api_->Plugins().Plugins()["plugins"].toArray()) {
            loaded.append(plugin.toObject()["id"].toString());
        }
        QVERIFY(loaded.contains(PLUGIN_ID));
        QVERIFY(loaded.contains("terminal"));
        QVERIFY(!loaded.contains("sessions"));
        QVERIFY(Api_->Plugins().Plugins()["problems"].toArray().isEmpty());
        // The panel of the built-in list is not created, nor its type known.
        QVERIFY(!Loader_->Info(LIST));
        QVERIFY(!Loader_->PanelTypes().contains(LIST));
        QCOMPARE(PanelsOfType(LIST), 0);
        QVERIFY(Root()->findChild<QQuickItem*>("sessionList") == nullptr);
        QCOMPARE(SlotCount(), 2);
        QVERIFY(Problems().isEmpty());

        // The list shows the sessions of roostd.
        QTRY_COMPARE_WITH_TIMEOUT(RowCount(list),
                                  static_cast<int>(ORDER.size()), TIMEOUT_MS);
        // Nothing is selected, and the terminal has no session.
        QCOMPARE(SelectedOf(list), QString());
        QVERIFY(TerminalOf(terminal) != nullptr);
        QVERIFY(TerminalOf(terminal)->property("session").toMap().isEmpty());

        // A tap on a row connects the terminal to that session.
        for (const QString& id : {BETA, GAMMA}) {
            TapSession(list, id);
            QTRY_COMPARE_WITH_TIMEOUT(SelectedOf(list), id, TIMEOUT_MS);
            QTRY_COMPARE_WITH_TIMEOUT(
                TerminalOf(terminal)->property("session").toMap().value("id"),
                QVariant(id), TIMEOUT_MS);
            QTRY_COMPARE_WITH_TIMEOUT(TerminalOf(terminal)->status(),
                                      QString("open"), TIMEOUT_MS);
            QTRY_VERIFY2_WITH_TIMEOUT(
                ScreenOf(terminal).contains(Greeting(id)),
                qPrintable(QString("%1x%2 cells, %3x%4 px:\n%5")
                               .arg(TerminalOf(terminal)->columns())
                               .arg(TerminalOf(terminal)->rows())
                               .arg(TerminalOf(terminal)->width())
                               .arg(TerminalOf(terminal)->height())
                               .arg(ScreenOf(terminal))),
                TIMEOUT_MS);
        }
        QVERIFY(Problems().isEmpty());
    }

    std::optional<ScopedEnv> Home_;
    std::optional<ScopedEnv> Socket_;
    std::optional<ScopedEnv> Config_;

private slots:
    void init() {
        Prepare();
        QVERIFY(QDir().mkpath(Dir_->filePath("home")));
        Home_.emplace("HOME", Dir_->filePath("home").toUtf8());
        Socket_.emplace("ROOST_SOCKET", SocketPath().toUtf8());
        Config_.emplace("ROOST_CONFIG", Dir_->filePath("config").toUtf8());
    }

    void cleanup() {
        Teardown();
        Config_.reset();
        Socket_.reset();
        Home_.reset();
    }

    void TheDefaultLayoutTakesTheListByItsOutput() {
        FailOnWarnings();
        QVERIFY(StartReplaced());
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        // No layout was stored, and the default one has the instance ids of
        // the window of before.
        VerifyReplaced("list", "terminal");
        QCOMPARE(Shell_->CurrentLayout().Root.Children[0].Panel, ALT_LIST);
        QVERIFY(StoredLayout().isEmpty());
    }

    void AStoredLayoutPointsAtTheNewList() {
        FailOnWarnings();
        const QJsonObject layout{
            {"version", 1},
            {"root",
             QJsonObject{
                 {"split", "h"},
                 {"children",
                  QJsonArray{
                      QJsonObject{{"instance", "side"},
                                  {"panel", ALT_LIST},
                                  {"links", QJsonObject{}}},
                      QJsonObject{{"instance", "screen"},
                                  {"panel", TERMINAL},
                                  {"links",
                                   QJsonObject{{"session",
                                                QJsonObject{{"from", "side"},
                                                            {"output",
                                                             "selected"}}}}}}}},
                 {"sizes", QJsonArray{200, "fill"}}}}};
        QVERIFY(StartReplaced(layout));
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        VerifyReplaced("side", "screen");
        QCOMPARE(SlotByInstance("side")->width(), 200.0);
    }

    void TheCommandToTheListReachesTheReplacingList() {
        // The keyboard is in no panel, so there is no list that feeds the
        // panel that has it; the command takes the list by its port.
        FailOnWarnings();
        QVERIFY(StartReplaced());
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("list") != nullptr &&
                                     PanelByInstance("terminal") != nullptr,
                                 TIMEOUT_MS);
        Window()->contentItem()->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(
            !SlotByInstance("list")->hasActiveFocus() &&
                !SlotByInstance("terminal")->hasActiveFocus(),
            TIMEOUT_MS);
        const QFuture<rpc::Result> done =
            Api_->Commands().Run("shell.focusList");
        QTRY_VERIFY_WITH_TIMEOUT(done.isFinished(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("list")->hasActiveFocus(),
                                 TIMEOUT_MS);

        // From the terminal, the list that feeds it is the one.
        SlotByInstance("terminal")->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("terminal")->hasActiveFocus(),
                                 TIMEOUT_MS);
        const QFuture<rpc::Result> again =
            Api_->Commands().Run("shell.focusList");
        QTRY_VERIFY_WITH_TIMEOUT(again.isFinished(), TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(SlotByInstance("list")->hasActiveFocus(),
                                 TIMEOUT_MS);
    }

    void ReadinessWaitsForTheReplacingListToRestore() {
        FailOnWarnings();
        QVERIFY(StartReplaced({}, true));
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(PanelByInstance("list") != nullptr &&
                                     PanelByInstance("terminal") != nullptr,
                                 TIMEOUT_MS);
        // Everything else is there; the list has not decided what to open.
        QTRY_VERIFY_WITH_TIMEOUT(Shell_->CheckReadiness().Reason.contains(
                                     "has not restored the opened session"),
                                 TIMEOUT_MS);
        QVERIFY(Shell_->CheckReadiness().Reason.contains(ALT_LIST));
        QVERIFY(!Shell_->CheckReadiness().Ready);
        PanelByInstance("list")->setProperty("restored", true);
        QTRY_VERIFY2_WITH_TIMEOUT(Shell_->CheckReadiness().Ready,
                                  qPrintable(Shell_->CheckReadiness().Reason),
                                  TIMEOUT_MS);
    }

    void WithoutTheSettingBothListsAreThere() {
        // The replacement is the setting: with the fixture installed and
        // `sessions` enabled, the built-in list is the first of two.
        FailOnWarnings();
        ListType_ = LIST;
        // After `sessions` in load order.
        QVERIFY(InstallFixture("zz-fixture"));
        QVERIFY(WriteFakeClaude(FAKE_CLAUDE));
        QVERIFY(StartDaemon());
        OpenWindow();
        if (QTest::currentTestFailed()) {
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(Loader_->Info(ALT_LIST).has_value(),
                                 TIMEOUT_MS);
        QVERIFY(Loader_->Info(LIST).has_value());
        QCOMPARE(SlotByInstance("list")->property("panelType").toString(),
                 LIST);
        // The one that is not placed is named.
        QTRY_VERIFY_WITH_TIMEOUT(Problems().join("\n").contains(ALT_LIST),
                                 TIMEOUT_MS);
    }
};

QTEST_MAIN(PluginReplaceTest)
#include "plugin_replace_test.moc"
