// The plugin store: documents survive a round trip, a save replaces the file
// by rename and never leaves half of one, and the config directory comes from
// the environment in the documented order.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <optional>

#include <sys/stat.h>
#include <unistd.h>

#include "core/store.hpp"
#include "helpers/scoped_env.hpp"

using roost::Store;
using roost::testing::ScopedEnv;

namespace {

ino_t InodeOf(const QString& path) {
    struct stat info{};
    if (::stat(QFile::encodeName(path).constData(), &info) != 0) {
        qFatal("cannot stat %s", qPrintable(path));
    }
    return info.st_ino;
}

QStringList FilesIn(const QString& directory) {
    return QDir(directory).entryList(QDir::AllEntries | QDir::Hidden |
                                     QDir::NoDotAndDotDot);
}

void WriteRaw(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        qFatal("cannot write %s", qPrintable(path));
    }
}

}  // namespace

class StoreTest : public QObject {
    Q_OBJECT

private slots:
    void RoundTrip();
    void MissingDocumentIsEmpty();
    void PluginsDoNotShareAFile();
    void SaveReplacesTheFileByRename();
    void FailedSaveKeepsTheOldDocument();
    void CreatesTheDirectoryOwnerOnly();
    void LeavesAnExistingDirectoryAlone();
    void CorruptFileIsAnError();
    void NonObjectIsAnError();
    void ValidPluginIds_data();
    void ValidPluginIds();
    void InvalidPluginIds_data();
    void InvalidPluginIds();
    void ReservedPluginIdIsRefused();
    void WindowDocumentsAreReadAndWritten();
    void MergePatchTable_data();
    void MergePatchTable();
    void MergeDiffAppliesBack_data();
    void MergeDiffAppliesBack();
    void ConfigDirectoryFromRoostConfig();
    void ConfigDirectoryFromXdg();
    void ConfigDirectoryIgnoresRelativeXdg();
    void ConfigDirectoryFromHome();
    void ConfigDirectoryCleansRoostConfig();
    void ConfigDirectoryNeedsAHome_data();
    void ConfigDirectoryNeedsAHome();
    void FilesAreOwnerOnly();
    void TargetThatIsADirectory();
    void UnreadableFileIsAnError();
    void DirectoryMustBeAbsolute();
};

void StoreTest::RoundTrip() {
    QTemporaryDir root;
    const Store store(root.path());
    const QJsonObject document{
        {QStringLiteral("pins"), QJsonArray{QStringLiteral("a"), 2}},
        {QStringLiteral("name"), QStringLiteral("сессия")},
    };
    QVERIFY(store.Save(QStringLiteral("claude"), document));
    QCOMPARE(store.Load(QStringLiteral("claude")).value(), document);
}

void StoreTest::MissingDocumentIsEmpty() {
    QTemporaryDir root;
    const Store store(root.filePath(QStringLiteral("not-yet")));
    QCOMPARE(store.Load(QStringLiteral("claude")).value(), QJsonObject());
    QVERIFY(!QDir(store.Directory()).exists());
}

void StoreTest::PluginsDoNotShareAFile() {
    QTemporaryDir root;
    const Store store(root.path());
    QVERIFY(store.Save(QStringLiteral("a"), {{QStringLiteral("v"), 1}}));
    QVERIFY(store.Save(QStringLiteral("b"), {{QStringLiteral("v"), 2}}));
    QCOMPARE(store.Load(QStringLiteral("a")).value()[QStringLiteral("v")], 1);
    QCOMPARE(FilesIn(root.path()), QStringList({"a.json", "b.json"}));
}

// A reader of the file sees the old document or the new one: the new bytes go
// to another file that takes the old one's name, so the inode changes.
void StoreTest::SaveReplacesTheFileByRename() {
    QTemporaryDir root;
    const Store store(root.path());
    const QString path = root.filePath(QStringLiteral("claude.json"));
    QVERIFY(store.Save(QStringLiteral("claude"), {{QStringLiteral("v"), 1}}));
    const ino_t before = InodeOf(path);

    QVERIFY(store.Save(QStringLiteral("claude"), {{QStringLiteral("v"), 2}}));
    QVERIFY(InodeOf(path) != before);
    QCOMPARE(FilesIn(root.path()), QStringList({"claude.json"}));
}

void StoreTest::FailedSaveKeepsTheOldDocument() {
    if (::geteuid() == 0) {
        QSKIP("root writes through directory permissions");
    }
    QTemporaryDir root;
    const Store store(root.path());
    const QJsonObject old{{QStringLiteral("v"), 1}};
    QVERIFY(store.Save(QStringLiteral("claude"), old));

    QVERIFY(
        QFile::setPermissions(root.path(), QFile::ReadOwner | QFile::ExeOwner));
    const auto saved =
        store.Save(QStringLiteral("claude"), {{QStringLiteral("v"), 2}});
    QVERIFY(QFile::setPermissions(
        root.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    QVERIFY(!saved);
    QVERIFY2(saved.error().contains(QStringLiteral("claude.json")),
             qPrintable(saved.error()));
    QCOMPARE(store.Load(QStringLiteral("claude")).value(), old);
    QCOMPARE(FilesIn(root.path()), QStringList({"claude.json"}));
}

void StoreTest::CreatesTheDirectoryOwnerOnly() {
    QTemporaryDir root;
    const QString directory = root.filePath(QStringLiteral("config/roost"));
    QVERIFY(Store(directory).Save(QStringLiteral("claude"), {}));

    struct stat info{};
    QCOMPARE(::stat(QFile::encodeName(directory).constData(), &info), 0);
    QCOMPARE(info.st_mode & 0777U, 0700U);
}

void StoreTest::LeavesAnExistingDirectoryAlone() {
    QTemporaryDir root;
    const QFile::Permissions open = QFile::ReadOwner | QFile::WriteOwner |
                                    QFile::ExeOwner | QFile::ReadOther |
                                    QFile::ExeOther;
    QVERIFY(QFile::setPermissions(root.path(), open));
    QVERIFY(Store(root.path()).Save(QStringLiteral("claude"), {}));
    QCOMPARE(QFile::permissions(root.path()) &
                 ~QFile::Permissions(QFile::ReadUser | QFile::WriteUser |
                                     QFile::ExeUser),
             open);
}

void StoreTest::CorruptFileIsAnError() {
    QTemporaryDir root;
    WriteRaw(root.filePath(QStringLiteral("claude.json")), R"({"pins": [)");
    const auto loaded = Store(root.path()).Load(QStringLiteral("claude"));
    QVERIFY(!loaded);
    QVERIFY2(loaded.error().contains(QStringLiteral("claude.json")),
             qPrintable(loaded.error()));
}

void StoreTest::NonObjectIsAnError() {
    QTemporaryDir root;
    WriteRaw(root.filePath(QStringLiteral("claude.json")), "[1, 2]");
    QVERIFY(!Store(root.path()).Load(QStringLiteral("claude")));
}

void StoreTest::ValidPluginIds_data() {
    QTest::addColumn<QString>("id");
    for (const char* id :
         {"claude", "a", "0x", "pr-watch", "my_plugin.v2", "a..b"}) {
        QTest::newRow(id) << QString::fromLatin1(id);
    }
    QTest::newRow("64 characters") << QString(64, u'a');
}

void StoreTest::ValidPluginIds() {
    QFETCH(QString, id);
    QTemporaryDir root;
    const Store store(root.path());
    QVERIFY(Store::IsValidPluginId(id));
    QVERIFY(store.Save(id, {{QStringLiteral("id"), id}}));
    QCOMPARE(FilesIn(root.path()), QStringList({id + QStringLiteral(".json")}));
}

void StoreTest::InvalidPluginIds_data() {
    QTest::addColumn<QString>("id");
    for (const char* id : {"", ".", "..", ".hidden", "-a", "_a", "a/b", "../a",
                           "a\\b", "Claude", "a b", "a:b", "плагин"}) {
        QTest::newRow(id) << QString::fromUtf8(id);
    }
    QTest::newRow("65 characters") << QString(65, u'a');
}

void StoreTest::InvalidPluginIds() {
    QFETCH(QString, id);
    QTemporaryDir root;
    const Store store(root.filePath(QStringLiteral("roost")));
    QVERIFY(!Store::IsValidPluginId(id));
    QVERIFY(!store.Save(id, {}));
    QVERIFY(!store.Load(id));
    QVERIFY(FilesIn(root.path()).isEmpty());
}

void StoreTest::ReservedPluginIdIsRefused() {
    QTemporaryDir root;
    const Store store(root.filePath(QStringLiteral("roost")));
    const QString id = QStringLiteral("keymap");
    QVERIFY(Store::IsReservedPluginId(id));
    QVERIFY(!Store::IsReservedPluginId(QStringLiteral("keymaps")));
    QVERIFY(!store.Save(id, {}));
    QVERIFY(!store.Load(id));
    QVERIFY(FilesIn(root.path()).isEmpty());
}

void StoreTest::WindowDocumentsAreReadAndWritten() {
    QTemporaryDir root;
    const Store store(root.filePath(QStringLiteral("roost")));
    for (const char* name : {"layout", "settings", "updates"}) {
        const QString id = QString::fromLatin1(name);
        QVERIFY(Store::IsReservedPluginId(id));
        QVERIFY(Store::IsWindowDocumentId(id));
        QCOMPARE(store.Load(id).value(), QJsonObject());
        const QJsonObject document{{QStringLiteral("version"), 1}};
        QVERIFY(store.Save(id, document));
        QCOMPARE(store.Load(id).value(), document);
    }
    QVERIFY(!Store::IsWindowDocumentId(QStringLiteral("keymap")));
    QVERIFY(!Store::IsWindowDocumentId(QStringLiteral("layouts")));
}

namespace {

QJsonObject ObjectOf(const char* json) {
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

}  // namespace

void StoreTest::MergePatchTable_data() {
    QTest::addColumn<QByteArray>("target");
    QTest::addColumn<QByteArray>("patch");
    QTest::addColumn<QByteArray>("result");
    // The examples of RFC 7386, appendix A, and the rules they show.
    const auto row = [](const char* name, const char* target, const char* patch,
                        const char* result) {
        QTest::newRow(name)
            << QByteArray(target) << QByteArray(patch) << QByteArray(result);
    };
    row("replace a value", R"({"a":"b"})", R"({"a":"c"})", R"({"a":"c"})");
    row("add a key", R"({"a":"b"})", R"({"b":"c"})", R"({"a":"b","b":"c"})");
    row("null removes a key", R"({"a":"b"})", R"({"a":null})", R"({})");
    row("null removes one of two", R"({"a":"b","b":"c"})", R"({"a":null})",
        R"({"b":"c"})");
    row("null for a missing key", R"({"a":"b"})", R"({"x":null})",
        R"({"a":"b"})");
    row("an array is replaced", R"({"a":["b"]})", R"({"a":"c"})",
        R"({"a":"c"})");
    row("a value is replaced by an array", R"({"a":"c"})", R"({"a":["b"]})",
        R"({"a":["b"]})");
    row("arrays are not merged", R"({"a":[1,2,3]})", R"({"a":[9]})",
        R"({"a":[9]})");
    row("nested objects merge", R"({"a":{"b":"c"}})",
        R"({"a":{"b":"d","c":null}})", R"({"a":{"b":"d"}})");
    row("nested objects keep the rest", R"({"a":{"b":1,"c":2},"d":3})",
        R"({"a":{"c":9}})", R"({"a":{"b":1,"c":9},"d":3})");
    row("nested null removes the key", R"({"a":{"b":1}})",
        R"({"a":{"b":null}})", R"({"a":{}})");
    row("an array of objects is replaced", R"({"a":[{"b":"c"}]})",
        R"({"a":[1]})", R"({"a":[1]})");
    row("an object replaces a scalar", R"({"a":1})", R"({"a":{"b":2}})",
        R"({"a":{"b":2}})");
    row("an object replaces an array", R"({"a":[1]})", R"({"a":{"b":2}})",
        R"({"a":{"b":2}})");
    row("nulls inside a new object are dropped", R"({})",
        R"({"a":{"b":{"c":null,"d":1}}})", R"({"a":{"b":{"d":1}}})");
    row("an empty patch changes nothing", R"({"a":1})", R"({})", R"({"a":1})");
    row("a patch into an empty document", R"({})", R"({"a":1})", R"({"a":1})");
    row("false and 0 are values, not removals", R"({"a":true,"b":1})",
        R"({"a":false,"b":0})", R"({"a":false,"b":0})");
}

void StoreTest::MergePatchTable() {
    QFETCH(QByteArray, target);
    QFETCH(QByteArray, patch);
    QFETCH(QByteArray, result);
    QJsonObject document = ObjectOf(target);
    roost::ApplyMergePatch(document, ObjectOf(patch));
    QCOMPARE(document, ObjectOf(result));
}

void StoreTest::MergeDiffAppliesBack_data() {
    QTest::addColumn<QByteArray>("before");
    QTest::addColumn<QByteArray>("after");
    const auto row = [](const char* name, const char* before,
                        const char* after) {
        QTest::newRow(name) << QByteArray(before) << QByteArray(after);
    };
    row("equal", R"({"a":1})", R"({"a":1})");
    row("both empty", R"({})", R"({})");
    row("from empty", R"({})", R"({"a":1,"b":{"c":[1,2]}})");
    row("to empty", R"({"a":1,"b":{"c":2}})", R"({})");
    row("a key added", R"({"a":1})", R"({"a":1,"b":2})");
    row("a key removed", R"({"a":1,"b":2})", R"({"a":1})");
    row("a value changed", R"({"a":1})", R"({"a":2})");
    row("a type changed", R"({"a":1})", R"({"a":"x"})");
    row("an array changed", R"({"a":[1,2,3]})", R"({"a":[1,2]})");
    row("an array becomes an object", R"({"a":[1]})", R"({"a":{"b":1}})");
    row("an object becomes an array", R"({"a":{"b":1}})", R"({"a":[1]})");
    row("an object becomes a scalar", R"({"a":{"b":1}})", R"({"a":1})");
    row("an object becomes empty", R"({"a":{"b":1}})", R"({"a":{}})");
    row("a scalar becomes an empty object", R"({"a":1})", R"({"a":{}})");
    row("a nested key changed", R"({"a":{"b":1,"c":2},"d":3})",
        R"({"a":{"b":1,"c":5},"d":3})");
    row("deep", R"({"a":{"b":{"c":{"d":1,"e":2}}},"f":0})",
        R"({"a":{"b":{"c":{"d":1,"g":[{"h":true}]}}},"f":0})");
    row("false and 0", R"({"a":true,"b":1})", R"({"a":false,"b":0})");
}

void StoreTest::MergeDiffAppliesBack() {
    QFETCH(QByteArray, before);
    QFETCH(QByteArray, after);
    const QJsonObject a = ObjectOf(before);
    const QJsonObject b = ObjectOf(after);
    const QJsonObject diff = roost::MergeDiff(a, b);
    QJsonObject applied = a;
    roost::ApplyMergePatch(applied, diff);
    QCOMPARE(applied, b);
    // Nothing changed, nothing in the patch.
    if (a == b) {
        QVERIFY(diff.isEmpty());
    }
}

void StoreTest::ConfigDirectoryFromRoostConfig() {
    const ScopedEnv roost("ROOST_CONFIG", QByteArray("/somewhere/state"));
    const ScopedEnv xdg("XDG_CONFIG_HOME", QByteArray("/xdg"));
    QCOMPARE(roost::ConfigDirectory().value(),
             QStringLiteral("/somewhere/state"));
}

void StoreTest::ConfigDirectoryFromXdg() {
    const ScopedEnv roost("ROOST_CONFIG", std::nullopt);
    const ScopedEnv xdg("XDG_CONFIG_HOME", QByteArray("/xdg/"));
    QCOMPARE(roost::ConfigDirectory().value(), QStringLiteral("/xdg/roost"));
}

void StoreTest::ConfigDirectoryIgnoresRelativeXdg() {
    const ScopedEnv roost("ROOST_CONFIG", std::nullopt);
    const ScopedEnv xdg("XDG_CONFIG_HOME", QByteArray("relative"));
    const ScopedEnv home("HOME", QByteArray("/home/someone"));
    QCOMPARE(roost::ConfigDirectory().value(),
             QStringLiteral("/home/someone/.config/roost"));
}

void StoreTest::ConfigDirectoryFromHome() {
    const ScopedEnv roost("ROOST_CONFIG", std::nullopt);
    const ScopedEnv xdg("XDG_CONFIG_HOME", std::nullopt);
    const ScopedEnv home("HOME", QByteArray("/home/someone"));
    QCOMPARE(roost::ConfigDirectory().value(),
             QStringLiteral("/home/someone/.config/roost"));
}

void StoreTest::ConfigDirectoryCleansRoostConfig() {
    {
        const ScopedEnv roost("ROOST_CONFIG", QByteArray("/a/b//"));
        QCOMPARE(roost::ConfigDirectory().value(), QStringLiteral("/a/b"));
    }
    const ScopedEnv roost("ROOST_CONFIG", QByteArray("state"));
    QCOMPARE(roost::ConfigDirectory().value(),
             QDir::current().filePath(QStringLiteral("state")));
}

void StoreTest::ConfigDirectoryNeedsAHome_data() {
    QTest::addColumn<std::optional<QByteArray>>("home");
    QTest::newRow("unset") << std::optional<QByteArray>();
    QTest::newRow("empty") << std::optional(QByteArray());
    QTest::newRow("root") << std::optional(QByteArray("/"));
    QTest::newRow("relative") << std::optional(QByteArray("me"));
}

void StoreTest::ConfigDirectoryNeedsAHome() {
    QFETCH(std::optional<QByteArray>, home);
    const ScopedEnv roost("ROOST_CONFIG", std::nullopt);
    const ScopedEnv xdg("XDG_CONFIG_HOME", std::nullopt);
    const ScopedEnv scopedHome("HOME", home);
    const auto directory = roost::ConfigDirectory();
    QVERIFY2(!directory, qPrintable(directory.value_or(QString())));
    QVERIFY(directory.error().contains(QStringLiteral("ROOST_CONFIG")));
}

// Plugin state may hold tokens: 0600 in a directory someone left open, and
// 0600 again over a file that was not.
void StoreTest::FilesAreOwnerOnly() {
    QTemporaryDir root;
    QVERIFY(QFile::setPermissions(
        root.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                         QFile::ReadOther | QFile::ExeOther));
    const Store store(root.path());
    const QString path = root.filePath(QStringLiteral("claude.json"));
    auto mode = [&path] {
        struct stat info{};
        if (::stat(QFile::encodeName(path).constData(), &info) != 0) {
            qFatal("cannot stat %s", qPrintable(path));
        }
        return info.st_mode & 0777U;
    };

    QVERIFY(store.Save(QStringLiteral("claude"), {}));
    QCOMPARE(mode(), 0600U);

    QVERIFY(QFile::setPermissions(
        path, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther));
    QCOMPARE(mode(), 0604U);
    QVERIFY(store.Save(QStringLiteral("claude"), {{QStringLiteral("v"), 1}}));
    QCOMPARE(mode(), 0600U);
}

void StoreTest::TargetThatIsADirectory() {
    QTemporaryDir root;
    QVERIFY(QDir(root.path()).mkdir(QStringLiteral("claude.json")));
    const Store store(root.path());
    QVERIFY(!store.Save(QStringLiteral("claude"), {}));
    QVERIFY(!store.Load(QStringLiteral("claude")));
    QCOMPARE(FilesIn(root.path()), QStringList({"claude.json"}));
}

void StoreTest::UnreadableFileIsAnError() {
    if (::geteuid() == 0) {
        QSKIP("root reads through file permissions");
    }
    QTemporaryDir root;
    const QString path = root.filePath(QStringLiteral("claude.json"));
    WriteRaw(path, "{}");
    QVERIFY(QFile::setPermissions(path, QFile::Permissions()));
    const auto loaded = Store(root.path()).Load(QStringLiteral("claude"));
    QVERIFY(!loaded);
    QVERIFY2(loaded.error().contains(QStringLiteral("claude.json")),
             qPrintable(loaded.error()));
}

void StoreTest::DirectoryMustBeAbsolute() {
    for (const QString& directory : {QString(), QStringLiteral("relative")}) {
        const Store store(directory);
        QVERIFY(!store.Save(QStringLiteral("claude"), {}));
        QVERIFY(!store.Load(QStringLiteral("claude")));
    }
    QVERIFY(!QFile::exists(QStringLiteral("relative")));
    QVERIFY(!QFile::exists(QStringLiteral("/claude.json")));
}

QTEST_GUILESS_MAIN(StoreTest)
#include "store_test.moc"
