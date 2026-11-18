// Plugin manifests: what is accepted and refused, where plugins are looked
// for, and who wins a conflict.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include "helpers/scoped_env.hpp"
#include "roostd/manifest.hpp"

using namespace roost;
using roost::testing::ScopedEnv;

namespace {

const QString DIRECTORY = QStringLiteral("/plugins/claude");

// The error to show when an expected value is missing. QVERIFY2 computes its
// message before the check, so it must not call error() on a value.
template <typename TExpected>
QString Why(const TExpected& expected) {
    return expected ? QString() : expected.error();
}

QByteArray Json(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject Minimal(const QString& id = QStringLiteral("claude")) {
    return {{"id", id}, {"version", "0.1.0"}, {"api", "0"}};
}

QJsonObject With(QJsonObject object, const QString& key,
                 const QJsonValue& value) {
    object.insert(key, value);
    return object;
}

PluginManifest Plugin(const QString& id, const QStringList& commands,
                      const QStringList& keys = {}) {
    PluginManifest manifest;
    manifest.Id = id;
    manifest.Directory = "/plugins/" + id;
    for (const QString& command : commands) {
        manifest.Commands.append({.Id = command, .Title = command});
    }
    for (const QString& key : keys) {
        manifest.Keybindings.append({.Command = id + ".x", .Key = key});
    }
    return manifest;
}

void WriteFile(const QString& path, const QByteArray& content) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
}

}  // namespace

class ManifestTest : public QObject {
    Q_OBJECT

private slots:
    void TheDesignExampleParses() {
        // docs/design.md §5.
        const QByteArray json = R"({
          "id": "claude",
          "version": "0.1.0",
          "api": "0",
          "backend": { "command": ["backend/claude-adapter"] },
          "contributes": {
            "agents": [{ "id": "claude", "capabilities": ["backgroundDaemon", "nativeTuiAttach"] }],
            "panels": [{ "id": "claude.transcript", "qml": "qml/Transcript.qml",
                         "inputs": { "session": "SessionRef" }, "outputs": {} }],
            "commands": [{ "id": "claude.newSession", "title": "New Claude session" }],
            "keybindings": [{ "command": "claude.newSession", "key": "Ctrl+N" }],
            "settings": "qml/Settings.qml"
          }
        })";
        const auto manifest = ParseManifest(json, DIRECTORY);
        QVERIFY2(manifest, qPrintable(Why(manifest)));
        QCOMPARE(manifest->Id, QString("claude"));
        QCOMPARE(manifest->Version, QString("0.1.0"));
        QCOMPARE(manifest->Directory, DIRECTORY);
        QCOMPARE(manifest->Backend,
                 QStringList{DIRECTORY + "/backend/claude-adapter"});
        QCOMPARE(manifest->Commands.size(), 1);
        QCOMPARE(manifest->Commands.front().Id, QString("claude.newSession"));
        QCOMPARE(manifest->Commands.front().Title,
                 QString("New Claude session"));
        QCOMPARE(manifest->Keybindings.size(), 1);
        QCOMPARE(manifest->Keybindings.front().Command,
                 QString("claude.newSession"));
        QCOMPARE(manifest->Keybindings.front().Key, QString("Ctrl+N"));
        QCOMPARE(manifest->Contributes.value("settings").toString(),
                 QString("qml/Settings.qml"));
        QCOMPARE(manifest->Contributes.value("panels").toArray().size(), 1);
        QCOMPARE(manifest->Agents.size(), 1);
        QCOMPARE(manifest->Agents.front().Id, QString("claude"));
        QCOMPARE(manifest->Agents.front().Capabilities,
                 (QStringList{"backgroundDaemon", "nativeTuiAttach"}));
    }

    void AgentMayLeaveCapabilitiesOut() {
        QJsonObject json = With(Minimal(), "backend",
                                QJsonObject{{"command", QJsonArray{"run"}}});
        json.insert(
            "contributes",
            QJsonObject{{"agents", QJsonArray{QJsonObject{{"id", "codex"}}}}});
        const auto manifest = ParseManifest(Json(json), DIRECTORY);
        QVERIFY2(manifest, qPrintable(Why(manifest)));
        QCOMPARE(manifest->Agents.size(), 1);
        QCOMPARE(manifest->Agents.front().Id, QString("codex"));
        QVERIFY(manifest->Agents.front().Capabilities.isEmpty());
    }

    void MinimalManifestHasNothingToRun() {
        const auto manifest = ParseManifest(Json(Minimal()), DIRECTORY);
        QVERIFY2(manifest, qPrintable(Why(manifest)));
        QVERIFY(manifest->Backend.isEmpty());
        QVERIFY(manifest->Commands.isEmpty());
        QVERIFY(manifest->Keybindings.isEmpty());
        QVERIFY(manifest->Agents.isEmpty());
        QVERIFY(manifest->Contributes.isEmpty());
    }

    void UnknownKeysAreIgnored() {
        QJsonObject json = With(Minimal(), "future", QJsonObject{{"a", 1}});
        json.insert("contributes", QJsonObject{{"themes", QJsonArray{1}}});
        QVERIFY(ParseManifest(Json(json), DIRECTORY));
    }

    void NativeWithAQtAndAnImportPathParses() {
        const QString qt =
            QStringLiteral("%1.%2").arg(QT_VERSION_MAJOR).arg(QT_VERSION_MINOR);
        const auto manifest = ParseManifest(
            Json(With(Minimal(), "native",
                      QJsonObject{{"qt", qt}, {"qmlImportPath", "qml"}})),
            DIRECTORY);
        QVERIFY2(manifest, qPrintable(Why(manifest)));
        QVERIFY(manifest->Native.has_value());
        QCOMPARE(manifest->Native->Qt, qt);
        QCOMPARE(manifest->Native->QmlImportPath, QString("qml"));
        // An older minor of the same Qt is as good.
        if (QT_VERSION_MINOR > 0) {
            QVERIFY(ParseManifest(
                Json(With(Minimal(), "native",
                          QJsonObject{{"qt", QStringLiteral("%1.0").arg(
                                                 QT_VERSION_MAJOR)}})),
                DIRECTORY));
        }
        const auto without = ParseManifest(Json(Minimal()), DIRECTORY);
        QVERIFY(without);
        QVERIFY(!without->Native.has_value());
    }

    void NativeImportPathMustStayInsideThePlugin_data() {
        QTest::addColumn<QString>("path");
        QTest::newRow("parent") << "../x";
        QTest::newRow("sibling with a common prefix") << "../claude2/qml";
        QTest::newRow("deeper and out") << "qml/../../x";
        QTest::newRow("absolute") << "/abs";
        QTest::newRow("the directory itself") << ".";
        QTest::newRow("empty") << "";
    }

    void NativeImportPathMustStayInsideThePlugin() {
        QFETCH(QString, path);
        const auto manifest =
            ParseManifest(Json(With(Minimal(), "native",
                                    QJsonObject{{"qmlImportPath", path}})),
                          DIRECTORY);
        QVERIFY(!manifest);
        QVERIFY2(manifest.error().contains("native.qmlImportPath"),
                 qPrintable(manifest.error()));
    }

    void NativeQtOfAnotherMajorOrNewerMinorIsRefused_data() {
        QTest::addColumn<QString>("qt");
        QTest::newRow("older major")
            << QStringLiteral("%1.0").arg(QT_VERSION_MAJOR - 1);
        QTest::newRow("newer major")
            << QStringLiteral("%1.0").arg(QT_VERSION_MAJOR + 1);
        QTest::newRow("newer minor") << QStringLiteral("%1.%2")
                                            .arg(QT_VERSION_MAJOR)
                                            .arg(QT_VERSION_MINOR + 1);
        QTest::newRow("a patch is not part of it")
            << QStringLiteral("%1.%2.1")
                   .arg(QT_VERSION_MAJOR)
                   .arg(QT_VERSION_MINOR);
        QTest::newRow("a major alone") << QString::number(QT_VERSION_MAJOR);
        QTest::newRow("text") << "latest";
    }

    void NativeQtOfAnotherMajorOrNewerMinorIsRefused() {
        QFETCH(QString, qt);
        const auto manifest = ParseManifest(
            Json(With(Minimal(), "native", QJsonObject{{"qt", qt}})),
            DIRECTORY);
        QVERIFY(!manifest);
        QVERIFY2(manifest.error().contains("native.qt"),
                 qPrintable(manifest.error()));
    }

    void NativeQtOfThisRoostIsAccepted() {
        // The edge of the rule: the same minor passes.
        const QString qt =
            QStringLiteral("%1.%2").arg(QT_VERSION_MAJOR).arg(QT_VERSION_MINOR);
        QVERIFY(ParseManifest(
            Json(With(Minimal(), "native", QJsonObject{{"qt", qt}})),
            DIRECTORY));
    }

    void NativeLibraryIsRefusedUntilTheSdk_data() {
        QTest::addColumn<QString>("key");
        QTest::newRow("library") << "library";
        QTest::newRow("sdk") << "sdk";
    }

    void NativeLibraryIsRefusedUntilTheSdk() {
        QFETCH(QString, key);
        const auto manifest = ParseManifest(
            Json(With(Minimal(), "native",
                      QJsonObject{{"qmlImportPath", "qml"}, {key, "lib/x"}})),
            DIRECTORY);
        QVERIFY(!manifest);
        QVERIFY2(manifest.error().contains("native." + key) &&
                     manifest.error().contains("SDK 0.1"),
                 qPrintable(manifest.error()));
    }

    void NativeOfTheWrongShapeIsRefused_data() {
        QTest::addColumn<QJsonValue>("native");
        QTest::addColumn<QString>("error");
        QTest::newRow("a string") << QJsonValue("qml") << "\"native\"";
        QTest::newRow("an array")
            << QJsonValue(QJsonArray{"qml"}) << "\"native\"";
        QTest::newRow("null") << QJsonValue(QJsonValue::Null) << "\"native\"";
        QTest::newRow("qt as a number")
            << QJsonValue(QJsonObject{{"qt", 6.8}}) << "native.qt";
        QTest::newRow("path as a number")
            << QJsonValue(QJsonObject{{"qmlImportPath", 1}})
            << "native.qmlImportPath";
        QTest::newRow("path as an array")
            << QJsonValue(QJsonObject{{"qmlImportPath", QJsonArray{"qml"}}})
            << "native.qmlImportPath";
    }

    void NativeOfTheWrongShapeIsRefused() {
        QFETCH(QJsonValue, native);
        QFETCH(QString, error);
        const auto manifest =
            ParseManifest(Json(With(Minimal(), "native", native)), DIRECTORY);
        QVERIFY(!manifest);
        QVERIFY2(manifest.error().contains(error),
                 qPrintable(manifest.error()));
    }

    void BackendIsResolvedInsideThePlugin_data() {
        QTest::addColumn<QJsonArray>("command");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("with arguments")
            << QJsonArray{"backend/run", "--flag", "/abs/arg"}
            << QStringList{DIRECTORY + "/backend/run", "--flag", "/abs/arg"};
        QTest::newRow("dot")
            << QJsonArray{"./run"} << QStringList{DIRECTORY + "/run"};
        QTest::newRow("dot dot that stays inside")
            << QJsonArray{"backend/../run"} << QStringList{DIRECTORY + "/run"};
        QTest::newRow("empty argument")
            << QJsonArray{"run", ""} << QStringList{DIRECTORY + "/run", ""};
    }

    void BackendIsResolvedInsideThePlugin() {
        QFETCH(QJsonArray, command);
        QFETCH(QStringList, expected);
        const auto manifest = ParseManifest(
            Json(With(Minimal(), "backend", QJsonObject{{"command", command}})),
            DIRECTORY);
        QVERIFY2(manifest, qPrintable(Why(manifest)));
        QCOMPARE(manifest->Backend, expected);
    }

    void DirectoryIsCleaned() {
        const auto manifest =
            ParseManifest(Json(Minimal()), "/plugins/./claude/");
        QVERIFY(manifest);
        QCOMPARE(manifest->Directory, DIRECTORY);
    }

    void BadManifestsAreRefused_data() {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<QString>("error");

        QTest::newRow("not JSON") << QByteArray("{") << "not valid JSON";
        QTest::newRow("array") << QByteArray("[]") << "not a JSON object";

        QTest::newRow("no id")
            << Json(With(Minimal(), "id", QJsonValue())) << "\"id\"";
        for (const char* id : {"Claude", "a/b", "", ".hidden", "a b"}) {
            QTest::addRow("id %s", id)
                << Json(Minimal(QString::fromUtf8(id))) << "\"id\"";
        }
        QTest::newRow("reserved id keymap")
            << Json(Minimal("keymap")) << "\"id\"";
        QTest::newRow("id as number")
            << Json(With(Minimal(), "id", 1)) << "\"id\"";

        QTest::newRow("no version")
            << Json(With(Minimal(), "version", QJsonValue())) << "\"version\"";
        QTest::newRow("empty version")
            << Json(With(Minimal(), "version", "")) << "\"version\"";
        QTest::newRow("version as number")
            << Json(With(Minimal(), "version", 1)) << "\"version\"";

        QTest::newRow("no api")
            << Json(With(Minimal(), "api", QJsonValue())) << "\"api\"";
        QTest::newRow("api 1")
            << Json(With(Minimal(), "api", "1")) << "\"api\"";
        QTest::newRow("api as number")
            << Json(With(Minimal(), "api", 0)) << "\"api\"";

        const auto backend = [](const QJsonValue& value) {
            return Json(With(Minimal(), "backend", value));
        };
        QTest::newRow("backend as array")
            << backend(QJsonArray{"run"}) << "\"backend\"";
        QTest::newRow("backend without command")
            << backend(QJsonObject{}) << "\"backend\"";
        QTest::newRow("backend command empty")
            << backend(QJsonObject{{"command", QJsonArray{}}}) << "\"backend\"";
        QTest::newRow("backend command as string")
            << backend(QJsonObject{{"command", "run"}}) << "\"backend\"";
        QTest::newRow("backend argument not a string")
            << backend(QJsonObject{{"command", QJsonArray{"run", 1}}})
            << "must hold strings";
        QTest::newRow("backend program empty")
            << backend(QJsonObject{{"command", QJsonArray{""}}})
            << "inside the plugin directory";
        QTest::newRow("absolute program")
            << backend(QJsonObject{{"command", QJsonArray{"/bin/sh"}}})
            << "inside the plugin directory";
        QTest::newRow("program outside")
            << backend(QJsonObject{{"command", QJsonArray{"../other/run"}}})
            << "inside the plugin directory";
        QTest::newRow("program outside, deeper")
            << backend(
                   QJsonObject{{"command", QJsonArray{"backend/../../x/run"}}})
            << "inside the plugin directory";
        QTest::newRow("program is the directory")
            << backend(QJsonObject{{"command", QJsonArray{"."}}})
            << "inside the plugin directory";
        QTest::newRow("sibling with a common prefix")
            << backend(QJsonObject{{"command", QJsonArray{"../claude2/run"}}})
            << "inside the plugin directory";

        const auto contributes = [](const QString& key,
                                    const QJsonValue& value) {
            return Json(
                With(Minimal(), "contributes", QJsonObject{{key, value}}));
        };
        QTest::newRow("contributes as array")
            << Json(With(Minimal(), "contributes", QJsonArray{}))
            << "\"contributes\"";
        QTest::newRow("commands as object")
            << contributes("commands", QJsonObject{}) << "contributes.commands";
        QTest::newRow("command without title")
            << contributes("commands", QJsonArray{QJsonObject{{"id", "a.b"}}})
            << "a command needs";
        QTest::newRow("command with empty title")
            << contributes("commands", QJsonArray{QJsonObject{{"id", "a.b"},
                                                              {"title", ""}}})
            << "a command needs";
        QTest::newRow("command with a bad id")
            << contributes("commands", QJsonArray{QJsonObject{{"id", "a..b"},
                                                              {"title", "T"}}})
            << "a command needs";
        QTest::newRow("command not an object")
            << contributes("commands", QJsonArray{"a.b"}) << "a command needs";
        QTest::newRow("keybindings as object")
            << contributes("keybindings", QJsonObject{})
            << "contributes.keybindings";
        QTest::newRow("keybinding without key")
            << contributes("keybindings",
                           QJsonArray{QJsonObject{{"command", "a.b"}}})
            << "a keybinding needs";
        QTest::newRow("keybinding without command")
            << contributes("keybindings",
                           QJsonArray{QJsonObject{{"key", "Ctrl+N"}}})
            << "a keybinding needs";
        QTest::newRow("keybinding with a blank key")
            << Json(With(
                   Minimal(), "contributes",
                   QJsonObject{
                       {"commands",
                        QJsonArray{QJsonObject{{"id", "a.b"}, {"title", "T"}}}},
                       {"keybindings", QJsonArray{QJsonObject{
                                           {"command", "a.b"}, {"key", " "}}}},
                   }))
            << "a keybinding needs";
        QTest::newRow("keybinding to another plugin's command")
            << contributes("keybindings",
                           QJsonArray{QJsonObject{{"command", "a.b"},
                                                  {"key", "Ctrl+N"}}})
            << "key Ctrl+N is bound to a.b, a command the plugin does not "
               "declare";
        QTest::newRow("agents as object")
            << contributes("agents", QJsonObject{}) << "contributes.agents";
        // With a backend, so that the agent itself is what is wrong.
        const auto agents = [](const QJsonValue& value) {
            return Json(With(With(Minimal(), "backend",
                                  QJsonObject{{"command", QJsonArray{"run"}}}),
                             "contributes", QJsonObject{{"agents", value}}));
        };
        QTest::newRow("agent without an id")
            << agents(QJsonArray{QJsonObject{}}) << "an agent needs an id";
        QTest::newRow("agent as string")
            << agents(QJsonArray{"claude"}) << "an agent needs an id";
        QTest::newRow("agent id in capitals")
            << agents(QJsonArray{QJsonObject{{"id", "Claude"}}})
            << "an agent needs an id";
        QTest::newRow("capabilities as string")
            << agents(QJsonArray{
                   QJsonObject{{"id", "claude"}, {"capabilities", "all"}}})
            << "the capabilities of agent claude must be an array";
        QTest::newRow("capability as number")
            << agents(QJsonArray{QJsonObject{{"id", "claude"},
                                             {"capabilities", QJsonArray{1}}}})
            << "the capabilities of agent claude must be strings";
        QTest::newRow("agent without a backend")
            << contributes("agents", QJsonArray{QJsonObject{{"id", "claude"}}})
            << "a plugin with agents needs a backend";
        QTest::newRow("panels as string")
            << contributes("panels", "x") << "contributes.panels";
        QTest::newRow("settings as number")
            << contributes("settings", 1) << "contributes.settings";
        QTest::newRow("settings outside")
            << contributes("settings", "../../etc/passwd")
            << "contributes.settings";
        QTest::newRow("settings absolute")
            << contributes("settings", "/etc/passwd") << "contributes.settings";
        QTest::newRow("settings empty")
            << contributes("settings", "") << "contributes.settings";
    }

    void BadManifestsAreRefused() {
        QFETCH(QByteArray, json);
        QFETCH(QString, error);
        const auto manifest = ParseManifest(json, DIRECTORY);
        QVERIFY(!manifest);
        QVERIFY2(manifest.error().contains(error),
                 qPrintable(manifest.error()));
    }

    void RelativeDirectoryIsRefused() {
        const auto manifest = ParseManifest(Json(Minimal()), "plugins/claude");
        QVERIFY(!manifest);
        QVERIFY(manifest.error().contains("not absolute"));
    }

    void LoadReadsPluginJson() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        WriteFile(dir.filePath("plugin.json"), Json(Minimal()));
        const auto manifest = LoadManifest(dir.path());
        QVERIFY2(manifest, qPrintable(Why(manifest)));
        QCOMPARE(manifest->Id, QString("claude"));
    }

    void LoadNamesTheFile() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto missing = LoadManifest(dir.path());
        QVERIFY(!missing);
        QVERIFY2(missing.error().contains(dir.filePath("plugin.json")),
                 qPrintable(missing.error()));

        WriteFile(dir.filePath("plugin.json"), "{");
        const auto broken = LoadManifest(dir.path());
        QVERIFY(!broken);
        QVERIFY2(broken.error().startsWith(dir.filePath("plugin.json")),
                 qPrintable(broken.error()));
    }

    void LoadRefusesAHugeFile() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        WriteFile(dir.filePath("plugin.json"),
                  QByteArray(2 * 1024 * 1024, ' '));
        const auto manifest = LoadManifest(dir.path());
        QVERIFY(!manifest);
        QVERIFY2(manifest.error().contains("larger than"),
                 qPrintable(manifest.error()));
    }

    // --- where plugins are looked for ------------------------------------

    void SearchPathComesFromTheEnvironment_data() {
        QTest::addColumn<QByteArray>("variable");
        QTest::addColumn<QStringList>("expected");
        const QString config = "/config/plugins";
        QTest::newRow("empty") << QByteArray() << QStringList{config};
        QTest::newRow("two")
            << QByteArray("/a:/b/") << QStringList{"/a", "/b", config};
        QTest::newRow("empty entries")
            << QByteArray("::/a::") << QStringList{"/a", config};
        QTest::newRow("relative entries are skipped")
            << QByteArray("rel:/a:./x") << QStringList{"/a", config};
        QTest::newRow("each directory once")
            << QByteArray("/a:/a/:/config/plugins:/b:/a")
            << QStringList{"/a", config, "/b"};
    }

    void SearchPathComesFromTheEnvironment() {
        QFETCH(QByteArray, variable);
        QFETCH(QStringList, expected);
        const ScopedEnv path("ROOST_PLUGINS_PATH", variable);
        QCOMPARE(PluginSearchPath("/config"), expected);
    }

    void SearchPathWithoutTheVariable() {
        const ScopedEnv path("ROOST_PLUGINS_PATH", std::nullopt);
        QCOMPARE(PluginSearchPath("/config"), QStringList{"/config/plugins"});
    }

    void PluginsAreFoundInOrder() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir root(dir.path());
        for (const char* path : {"first/b", "first/a", "first/Zed",
                                 "first/empty", "second/c", "second/a"}) {
            QVERIFY(root.mkpath(path));
        }
        for (const char* path :
             {"first/b/plugin.json", "first/a/plugin.json",
              "first/Zed/plugin.json", "second/c/plugin.json",
              "second/a/plugin.json"}) {
            WriteFile(root.filePath(path), "{}");
        }
        // A file named like a manifest is not a plugin directory.
        WriteFile(root.filePath("first/plugin.json"), "{}");
        QVERIFY(root.mkpath("first/dir/plugin.json"));

        const QStringList found = FindPluginDirectories(
            {root.filePath("first"), root.filePath("missing"),
             root.filePath("second")});
        // Case first: Z before a.
        QCOMPARE(found, (QStringList{
                            root.filePath("first/Zed"),
                            root.filePath("first/a"),
                            root.filePath("first/b"),
                            root.filePath("second/a"),
                            root.filePath("second/c"),
                        }));
    }

    // --- the registry ----------------------------------------------------

    void PluginsAreFoundById() {
        PluginRegistry registry;
        QVERIFY(registry.Add(Plugin("one", {"one.run"})));
        QVERIFY(registry.Add(Plugin("two", {"two.run"})));
        QCOMPARE(registry.Plugins().size(), size_t{2});
        QCOMPARE(registry.Plugins()[0].Id, QString("one"));
        QVERIFY(registry.Find("two"));
        QCOMPARE(registry.Find("two")->Directory, QString("/plugins/two"));
        QVERIFY(!registry.Find("three"));
        QCOMPARE(registry.CommandOwner("two.run")->Id, QString("two"));
        QVERIFY(!registry.CommandOwner("three.run"));
        QVERIFY(registry.Problems().isEmpty());
    }

    void FirstPluginWithAnIdWins() {
        PluginRegistry registry;
        QVERIFY(registry.Add(Plugin("one", {"one.run"})));
        PluginManifest again = Plugin("one", {"one.other"});
        again.Directory = "/elsewhere/one";
        QVERIFY(!registry.Add(again));
        QCOMPARE(registry.Plugins().size(), size_t{1});
        QCOMPARE(registry.Find("one")->Directory, QString("/plugins/one"));
        // Nothing of the refused plugin is registered.
        QVERIFY(!registry.CommandOwner("one.other"));
        QCOMPARE(registry.Problems().size(), 1);
        QVERIFY2(registry.Problems().front().contains("/elsewhere/one"),
                 qPrintable(registry.Problems().front()));
    }

    void FirstOwnerOfACommandWins() {
        PluginRegistry registry;
        QVERIFY(registry.Add(Plugin("one", {"shared.run", "one.run"})));
        QVERIFY(registry.Add(Plugin("two", {"shared.run", "two.run"})));
        QCOMPARE(registry.CommandOwner("shared.run")->Id, QString("one"));
        QCOMPARE(registry.CommandOwner("two.run")->Id, QString("two"));
        QCOMPARE(registry.Problems(),
                 QStringList{"command shared.run of plugin two is ignored: "
                             "plugin one has it"});
    }

    void CommandDeclaredTwiceIsAProblem() {
        PluginRegistry registry;
        QVERIFY(registry.Add(Plugin("one", {"one.run", "one.run"})));
        QCOMPARE(registry.CommandOwner("one.run")->Id, QString("one"));
        QCOMPARE(registry.Problems(),
                 QStringList{"command one.run of plugin one is ignored: "
                             "plugin one has it"});
    }

    void FirstBindingOfAKeyWins() {
        PluginRegistry registry;
        QVERIFY(registry.Add(Plugin("one", {"one.x"}, {"Ctrl+N"})));
        QVERIFY(registry.Add(Plugin("two", {"two.x"}, {"ctrl+n", "Ctrl+M"})));
        QCOMPARE(registry.KeyOwner("CTRL+N")->Id, QString("one"));
        QCOMPARE(registry.KeyOwner("Ctrl+M")->Id, QString("two"));
        QVERIFY(!registry.KeyOwner("Ctrl+K"));
        QCOMPARE(registry.Problems(),
                 QStringList{"key ctrl+n of plugin two is ignored: plugin one "
                             "binds it"});
    }

    void KeyBoundTwiceInOnePluginIsAProblem() {
        PluginRegistry registry;
        QVERIFY(registry.Add(Plugin("one", {"one.x"}, {"Ctrl+N", "ctrl+N"})));
        QCOMPARE(registry.KeyOwner("Ctrl+N")->Id, QString("one"));
        QCOMPARE(registry.Problems(),
                 QStringList{"key ctrl+N of plugin one is ignored: plugin one "
                             "binds it"});
    }

    void KeyOfALostCommandIsIgnored() {
        PluginRegistry registry;
        QVERIFY(registry.Add(Plugin("one", {"shared.run"})));
        PluginManifest two = Plugin("two", {"shared.run"});
        two.Keybindings.append({.Command = "shared.run", .Key = "Ctrl+Q"});
        QVERIFY(registry.Add(two));
        // Ctrl+Q would run plugin one's command.
        QVERIFY(!registry.KeyOwner("Ctrl+Q"));
        QCOMPARE(registry.Problems(),
                 (QStringList{
                     "command shared.run of plugin two is ignored: plugin one "
                     "has it",
                     "key Ctrl+Q of plugin two is ignored: its command "
                     "shared.run belongs to plugin one",
                 }));
    }

    void KeyOfAnUndeclaredCommandIsIgnored() {
        PluginRegistry registry;
        QVERIFY(registry.Add(Plugin("one", {"one.run"})));
        QVERIFY(registry.Add(Plugin("two", {}, {"Ctrl+Q"})));
        QVERIFY(!registry.KeyOwner("Ctrl+Q"));
        QCOMPARE(registry.Problems(),
                 QStringList{"key Ctrl+Q of plugin two is ignored: the plugin "
                             "has no command two.x"});
    }

    void FirstOwnerOfAnAgentWins() {
        PluginRegistry registry;
        PluginManifest one = Plugin("one", {});
        one.Agents = {{.Id = "claude", .Capabilities = {}},
                      {.Id = "one", .Capabilities = {}}};
        PluginManifest two = Plugin("two", {});
        two.Agents = {{.Id = "claude", .Capabilities = {}},
                      {.Id = "two", .Capabilities = {}}};
        QVERIFY(registry.Add(one));
        QVERIFY(registry.Add(two));
        QCOMPARE(registry.AgentOwner("claude")->Id, QString("one"));
        QCOMPARE(registry.AgentOwner("two")->Id, QString("two"));
        QVERIFY(!registry.AgentOwner("codex"));
        QCOMPARE(registry.Problems(),
                 QStringList{"agent claude of plugin two is ignored: plugin "
                             "one serves it"});
    }

    void AgentDeclaredTwiceIsAProblem() {
        PluginRegistry registry;
        PluginManifest one = Plugin("one", {});
        one.Agents = {{.Id = "claude", .Capabilities = {}},
                      {.Id = "claude", .Capabilities = {}}};
        QVERIFY(registry.Add(one));
        QCOMPARE(registry.AgentOwner("claude")->Id, QString("one"));
        QCOMPARE(registry.Problems(),
                 QStringList{"agent claude of plugin one is ignored: plugin "
                             "one serves it"});
    }

    void ARefusedPluginServesNoAgent() {
        PluginRegistry registry;
        QVERIFY(registry.Add(Plugin("one", {})));
        PluginManifest again = Plugin("one", {});
        again.Agents = {{.Id = "claude", .Capabilities = {}}};
        QVERIFY(!registry.Add(again));
        QVERIFY(!registry.AgentOwner("claude"));
    }

    void ProblemsKeepTheirOrder() {
        PluginRegistry registry;
        registry.AddProblem("broken manifest");
        QVERIFY(registry.Add(Plugin("one", {})));
        QVERIFY(!registry.Add(Plugin("one", {})));
        QCOMPARE(registry.Problems().size(), 2);
        QCOMPARE(registry.Problems().front(), QString("broken manifest"));
    }
};

QTEST_GUILESS_MAIN(ManifestTest)
#include "manifest_test.moc"
