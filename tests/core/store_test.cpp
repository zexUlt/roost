// The plugin store: documents survive a round trip, a save replaces the file
// by rename and never leaves half of one, and the config directory comes from
// the environment in the documented order.

#include <QDir>
#include <QFile>
#include <QJsonArray>
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
