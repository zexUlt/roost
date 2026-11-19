// The install layout: `cmake --install` of this build tree into a temporary
// prefix gives the files of the OS (a roost.app on macOS, the usr/ of an AppDir
// on Linux), the licence texts and no other file; then the installed roostd,
// with no ROOST_PLUGINS_PATH and a fake `claude` first on PATH, finds the
// built-in plugins next to itself and lists the sessions of the fake claude,
// and the installed window (`roost --smoke-sessions`) shows them.
//
// The test installs by itself (cmake and the build directory come in as
// compile definitions) instead of reading a prefix that an earlier ctest step
// made: it passes alone and under -j, and the prefix is its own.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

#include <optional>

#include "core/rpc_client.hpp"

using namespace roost;

namespace {

constexpr int TIMEOUT_MS = 10000;
constexpr int INSTALL_TIMEOUT_MS = 120000;
// The mode gives up after 20 s (src/roost/main.cpp), and the window starts.
constexpr int WINDOW_TIMEOUT_MS = 60000;

#ifdef Q_OS_MACOS
constexpr bool IS_MACOS = true;
#else
constexpr bool IS_MACOS = false;
#endif

// The run path of each installed binary, in order: ours and, when
// ROOST_INSTALL_RPATH_TO_QT is on, the directory of the Qt of the build
// (QT_RUN_PATH is empty when it is off, or when Qt is in a directory the
// linker searches anyway). CMake puts the Qt directory first on macOS and last
// on Linux. On Linux the plugin binaries sit deeper in lib/roost; on macOS
// the QML module, which is loaded through a symlink, has the paths for both
// ways dyld may resolve @loader_path.
QStringList WithQtRunPath(const QStringList& ours) {
    const QString qt = QStringLiteral(QT_RUN_PATH);
    if (qt.isEmpty()) {
        return ours;
    }
    return IS_MACOS ? QStringList{qt} + ours : ours + QStringList{qt};
}
const QStringList BINARY_RUN_PATH =
    WithQtRunPath({IS_MACOS ? QStringLiteral("@executable_path/../Frameworks")
                            : QStringLiteral("$ORIGIN/../lib")});
const QStringList ADAPTER_RUN_PATH =
    WithQtRunPath({IS_MACOS ? QStringLiteral("@executable_path/../Frameworks")
                            : QStringLiteral("$ORIGIN/../../../..")});
const QStringList MODULE_RUN_PATH = WithQtRunPath(
    IS_MACOS
        ? QStringList{QStringLiteral("@executable_path/../Frameworks"),
                      QStringLiteral("@loader_path/../../Frameworks"),
                      QStringLiteral("@loader_path/../../../../../Frameworks")}
        : QStringList{QStringLiteral("$ORIGIN/../../../../..")});

// A fake `claude` that prints agents.json next to it for `agents`.
constexpr const char* FAKE_CLAUDE = R"sh(#!/bin/sh
dir=$(dirname "$0")
case "$1" in
agents) cat "$dir/agents.json" ;;
*) exit 2 ;;
esac
)sh";

// Whether a roostd, perhaps still starting, answers hello at the path.
bool Greets(const QString& socketPath) {
    const QDeadlineTimer deadline(TIMEOUT_MS);
    while (!deadline.hasExpired()) {
        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(socketPath);
        if (QTest::qWaitFor([&] { return hello.isFinished(); }, TIMEOUT_MS) &&
            hello.result()) {
            return true;
        }
        QTest::qWait(20);
    }
    return false;
}

QByteArray ReadAll(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The target a symlink holds, as written (not resolved).
QString LinkTarget(const QString& path) {
    char buffer[4096];
    const ssize_t length =
        ::readlink(QFile::encodeName(path), buffer, sizeof buffer);
    return length < 0
               ? QString()
               : QString::fromLocal8Bit(buffer, static_cast<qsizetype>(length));
}

// The output of a program, or nullopt if it did not run or failed.
std::optional<QString> Output(const QString& program,
                              const QStringList& arguments) {
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForFinished(TIMEOUT_MS) ||
        process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        return std::nullopt;
    }
    return QString::fromUtf8(process.readAllStandardOutput());
}

// The run path entries of a binary, in order: the LC_RPATH commands of
// `otool -l` on macOS, the RUNPATH or RPATH of `readelf -d` on Linux.
QStringList ParseRunPath(const QString& dump) {
    QStringList entries;
    const QStringList lines = dump.split(u'\n');
    if (IS_MACOS) {
        static const QRegularExpression PATH(
            R"(^\s*path (.+) \(offset \d+\)$)");
        bool inRpath = false;
        for (const QString& line : lines) {
            if (line.trimmed() == QLatin1String("cmd LC_RPATH")) {
                inRpath = true;
            } else if (inRpath) {
                const auto match = PATH.match(line);
                if (match.hasMatch()) {
                    entries.append(match.captured(1));
                    inRpath = false;
                }
            }
        }
    } else {
        static const QRegularExpression RUNPATH(
            R"(\((RUNPATH|RPATH)\).*\[(.*)\])");
        for (const QString& line : lines) {
            const auto match = RUNPATH.match(line);
            if (match.hasMatch()) {
                entries += match.captured(2).split(u':', Qt::SkipEmptyParts);
            }
        }
    }
    return entries;
}

}  // namespace

class InstallLayoutTest : public QObject {
    Q_OBJECT

private:
    QString Prefix() const { return Prefix_.path(); }
    QString Path(const QString& relative) const {
        return Prefix_.filePath(relative);
    }

    // Where the parts are, for the OS.
    QString BinDir() const {
        return Path(IS_MACOS ? "roost.app/Contents/MacOS" : "bin");
    }
    QString PluginsDir() const {
        return Path(IS_MACOS ? "roost.app/Contents/Resources/roost-plugins"
                             : "lib/roost/plugins");
    }
    QString LicensesDir() const {
        return Path(IS_MACOS ? "roost.app/Contents/Resources/licenses"
                             : "share/licenses/roost");
    }
    QString SessionsModuleDir() const {
        return PluginsDir() + "/sessions/qml/RoostSessions";
    }

    // Files and links that make up the install, relative to the prefix.
    QSet<QString> ExpectedFiles() const {
        const QString plugins =
            QString(IS_MACOS ? "roost.app/Contents/Resources/roost-plugins"
                             : "lib/roost/plugins");
        const QString licenses =
            QString(IS_MACOS ? "roost.app/Contents/Resources/licenses"
                             : "share/licenses/roost");
        QSet<QString> files{
            plugins + "/claude/plugin.json",
            plugins + "/claude/backend/" + CLAUDE_ADAPTER_FILE,
            plugins + "/sessions/plugin.json",
            plugins + "/sessions/qml/SessionsPanel.qml",
            plugins + "/sessions/qml/RoostSessions/qmldir",
            plugins + "/sessions/qml/RoostSessions/sessions_qml.qmltypes",
            plugins + "/sessions/qml/RoostSessions/" + SESSIONS_MODULE_FILE,
            plugins + "/terminal/plugin.json",
            plugins + "/terminal/qml/TerminalPanel.qml",
            licenses + "/LICENSE",
            licenses + "/NOTICE",
            licenses + "/THIRD_PARTY_LICENSES",
            licenses + "/LGPL-3.0.txt",
            licenses + "/GPL-3.0.txt",
        };
        if (IS_MACOS) {
            files.unite({
                "roost.app/Contents/Info.plist",
                "roost.app/Contents/MacOS/roost",
                "roost.app/Contents/MacOS/roostd",
                QString("roost.app/Contents/MacOS/") + CLAUDE_ADAPTER_FILE,
                QString("roost.app/Contents/PlugIns/roost/") +
                    SESSIONS_MODULE_FILE,
            });
        } else {
            files.unite({
                "bin/roost",
                "bin/roostd",
                "share/applications/roost.desktop",
                "share/icons/hicolor/256x256/apps/roost.png",
            });
        }
        return files;
    }

    // The run path of the installed binary is exactly the expected list.
    static void CheckRunPath(const QString& tool, const QString& binary,
                             const QStringList& expected) {
        const auto dump = Output(tool, IS_MACOS ? QStringList{"-l", binary}
                                                : QStringList{"-d", binary});
        QVERIFY2(dump, qPrintable(binary));
        const QStringList actual = ParseRunPath(*dump);
        QVERIFY2(actual == expected,
                 qPrintable(binary + ": run path [" + actual.join(u'|') +
                            "], expected [" + expected.join(u'|') + "]"));
    }

    // The ids of the background sessions of the fixture the fake claude prints.
    static QStringList FixtureSessionIds() {
        QStringList ids;
        const QJsonArray agents =
            QJsonDocument::fromJson(ReadAll(FIXTURES_DIR "/claude_agents.json"))
                .array();
        for (const QJsonValue agent : agents) {
            if (agent.toObject().value("kind").toString() == "background") {
                ids.append(agent.toObject().value("id").toString());
            }
        }
        ids.sort();
        return ids;
    }

    QTemporaryDir Prefix_;

private slots:
    void initTestCase() {
        QVERIFY(Prefix_.isValid());
        QProcess install;
        install.setProcessChannelMode(QProcess::MergedChannels);
        install.start(CMAKE_COMMAND, {"--install", BUILD_DIR, "--prefix",
                                      Prefix(), "--component", "roost"});
        QVERIFY(install.waitForFinished(INSTALL_TIMEOUT_MS));
        const QString log = QString::fromUtf8(install.readAll());
        QVERIFY2(install.exitStatus() == QProcess::NormalExit &&
                     install.exitCode() == 0,
                 qPrintable(log));
    }

    // Exactly the files of the layout: nothing missing, and nothing else
    // (headers of a dependency, tests, templates).
    void HasTheFilesOfTheOs() {
        QSet<QString> found;
        QDirIterator iterator(
            Prefix(),
            QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
            QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            found.insert(QDir(Prefix()).relativeFilePath(iterator.next()));
        }
        const QSet<QString> expected = ExpectedFiles();
        QStringList missing = (expected - found).values();
        QStringList extra = (found - expected).values();
        missing.sort();
        extra.sort();
        QVERIFY2(missing.isEmpty(),
                 qPrintable("missing: " + missing.join(", ")));
        QVERIFY2(extra.isEmpty(),
                 qPrintable("unexpected: " + extra.join(", ")));
    }

    void ProgramsAreExecutable() {
        QStringList programs{
            BinDir() + "/roost", BinDir() + "/roostd",
            PluginsDir() + "/claude/backend/" + CLAUDE_ADAPTER_FILE};
        for (const QString& program : programs) {
            QVERIFY2(QFileInfo(program).isExecutable(), qPrintable(program));
        }
    }

    // macOS keeps code outside Resources and links it into the plugin
    // directories with relative links; Linux has the code in place.
    void PluginBinariesAreWhereTheManifestsExpectThem() {
        const QString adapter =
            PluginsDir() + "/claude/backend/" + CLAUDE_ADAPTER_FILE;
        const QString module = SessionsModuleDir() + "/" + SESSIONS_MODULE_FILE;
        if (!IS_MACOS) {
            QVERIFY(!QFileInfo(adapter).isSymLink());
            QVERIFY(!QFileInfo(module).isSymLink());
            return;
        }
        struct Link {
            QString Path;
            QString Real;
        };
        const QList<Link> links{
            {adapter, Path("roost.app/Contents/MacOS/") + CLAUDE_ADAPTER_FILE},
            {module,
             Path("roost.app/Contents/PlugIns/roost/") + SESSIONS_MODULE_FILE},
        };
        for (const Link& link : links) {
            QVERIFY2(QFileInfo(link.Path).isSymLink(), qPrintable(link.Path));
            const QString target = LinkTarget(link.Path);
            QVERIFY2(!QDir::isAbsolutePath(target),
                     qPrintable(link.Path + " -> " + target));
            QVERIFY2(QFileInfo::exists(link.Path),
                     qPrintable(link.Path + " dangles"));
            QCOMPARE(QFileInfo(link.Path).canonicalFilePath(),
                     QFileInfo(link.Real).canonicalFilePath());
        }
    }

    void LicencesAreInstalledWhole() {
        const QString source = SOURCE_DIR;
        for (const QString& name :
             {"LICENSE", "NOTICE", "THIRD_PARTY_LICENSES"}) {
            const QByteArray text = ReadAll(source + "/" + name);
            QVERIFY(!text.isEmpty());
            QCOMPARE(ReadAll(LicensesDir() + "/" + name), text);
        }
        const QByteArray lgpl = ReadAll(LicensesDir() + "/LGPL-3.0.txt");
        const QByteArray gpl = ReadAll(LicensesDir() + "/GPL-3.0.txt");
        QCOMPARE(lgpl, ReadAll(source + "/licenses/LGPL-3.0.txt"));
        QCOMPARE(gpl, ReadAll(source + "/licenses/GPL-3.0.txt"));
        QVERIFY(lgpl.trimmed().startsWith("GNU LESSER GENERAL PUBLIC LICENSE"));
        QVERIFY(lgpl.contains("Version 3, 29 June 2007"));
        QVERIFY(gpl.trimmed().startsWith("GNU GENERAL PUBLIC LICENSE"));
        QVERIFY(gpl.contains("Version 3, 29 June 2007"));
        // Whole texts, not a stub.
        QVERIFY(lgpl.size() > 5000);
        QVERIFY(gpl.size() > 30000);
    }

    void MacOsBundleHasItsInfoPlist() {
        if (!IS_MACOS) {
            QSKIP("macOS only");
        }
        const auto json =
            Output("/usr/bin/plutil", {"-convert", "json", "-o", "-",
                                       Path("roost.app/Contents/Info.plist")});
        QVERIFY(json);
        const QJsonObject plist =
            QJsonDocument::fromJson(json->toUtf8()).object();
        QCOMPARE(plist.value("CFBundleIdentifier").toString(),
                 QStringLiteral("io.github.zexult.roost"));
        QCOMPARE(plist.value("CFBundleExecutable").toString(),
                 QStringLiteral("roost"));
        QCOMPARE(plist.value("CFBundlePackageType").toString(),
                 QStringLiteral("APPL"));
        QCOMPARE(plist.value("CFBundleShortVersionString").toString(),
                 QStringLiteral(PROJECT_VERSION_STRING));
        QCOMPARE(plist.value("CFBundleVersion").toString(),
                 QStringLiteral(PROJECT_VERSION_STRING));
        QVERIFY2(QRegularExpression(R"(^\d+(\.\d+)*$)")
                     .match(plist.value("LSMinimumSystemVersion").toString())
                     .hasMatch(),
                 qPrintable(plist.value("LSMinimumSystemVersion").toString()));
        QVERIFY(QFileInfo(BinDir() + "/" +
                          plist.value("CFBundleExecutable").toString())
                    .isExecutable());
    }

    void LinuxHasItsDesktopFileAndIcon() {
        if (IS_MACOS) {
            QSKIP("Linux only");
        }
        const QStringList lines =
            QString::fromUtf8(ReadAll(Path("share/applications/roost.desktop")))
                .split(u'\n');
        QVERIFY(lines.contains("[Desktop Entry]"));
        QVERIFY(lines.contains("Type=Application"));
        QVERIFY(lines.contains("Exec=roost"));
        QVERIFY(lines.contains("Icon=roost"));
        // The icon the file names, by the icon theme rules, is a PNG.
        QVERIFY(ReadAll(Path("share/icons/hicolor/256x256/apps/roost.png"))
                    .startsWith("\x89PNG\r\n\x1a\n"));
    }

    // The installed binaries do not lean on the build tree.
    void BinariesCarryTheirRunPaths() {
        const QString tool =
            QStandardPaths::findExecutable(IS_MACOS ? "otool" : "readelf");
        // A missing tool fails: a CI image without it must be noticed.
        QVERIFY2(!tool.isEmpty(), "no otool or readelf to read the run paths");
        CheckRunPath(tool, BinDir() + "/roost", BINARY_RUN_PATH);
        CheckRunPath(tool, BinDir() + "/roostd", BINARY_RUN_PATH);
        CheckRunPath(
            tool,
            IS_MACOS ? BinDir() + "/" + CLAUDE_ADAPTER_FILE
                     : PluginsDir() + "/claude/backend/" + CLAUDE_ADAPTER_FILE,
            ADAPTER_RUN_PATH);
        CheckRunPath(tool,
                     IS_MACOS
                         ? Path("roost.app/Contents/PlugIns/roost/") +
                               SESSIONS_MODULE_FILE
                         : SessionsModuleDir() + "/" + SESSIONS_MODULE_FILE,
                     MODULE_RUN_PATH);
    }

    // The installed roostd, with nothing in the environment that names the
    // plugins: the built-ins are found next to the binary, load without a
    // problem, and the Claude plugin lists the sessions of the fake claude.
    void InstalledRoostdListsTheBuiltInsAndTheSessions() {
        QTemporaryDir state;
        QVERIFY(state.isValid());
        const QDir bin(state.filePath("bin"));
        QFile script(bin.filePath("claude"));
        QVERIFY(bin.mkpath("."));
        QVERIFY(script.open(QIODevice::WriteOnly));
        QVERIFY(script.write(FAKE_CLAUDE) > 0);
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                                      QFile::ExeOwner));
        script.close();
        QVERIFY(QFile::copy(FIXTURES_DIR "/claude_agents.json",
                            bin.filePath("agents.json")));

        const QString socket = state.filePath("roostd.sock");
        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("ROOST_SOCKET", socket);
        environment.insert("ROOST_CONFIG", state.filePath("config"));
        environment.remove("ROOST_PLUGINS_PATH");
        environment.insert("PATH",
                           bin.path() + u':' + environment.value("PATH"));
        QProcess daemon;
        daemon.setProcessEnvironment(environment);
        daemon.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        daemon.start(BinDir() + "/roostd", {});
        QVERIFY(daemon.waitForStarted(TIMEOUT_MS));
        const auto stop = qScopeGuard([&] {
            if (daemon.state() != QProcess::NotRunning) {
                daemon.terminate();
                if (!daemon.waitForFinished(TIMEOUT_MS)) {
                    daemon.kill();
                    daemon.waitForFinished(TIMEOUT_MS);
                }
            }
        });
        QVERIFY(Greets(socket));

        RpcClient client;
        QFuture<rpc::Result> hello = client.Connect(socket);
        QVERIFY(
            QTest::qWaitFor([&] { return hello.isFinished(); }, TIMEOUT_MS));
        QVERIFY(hello.result());

        const auto call = [&](const QString& method) {
            QFuture<rpc::Result> answer = client.Request(method, QJsonObject{});
            if (!QTest::qWaitFor([&] { return answer.isFinished(); },
                                 TIMEOUT_MS)) {
                return QJsonObject();
            }
            return answer.result() ? answer.result()->toObject()
                                   : QJsonObject();
        };

        const QJsonObject plugins = call("plugins.list");
        QVERIFY(!plugins.isEmpty());
        QCOMPARE(plugins.value("problems").toArray(), QJsonArray());
        QStringList ids;
        const QString installed = QFileInfo(PluginsDir()).canonicalFilePath();
        for (const QJsonValue plugin : plugins.value("plugins").toArray()) {
            ids.append(plugin.toObject().value("id").toString());
            // From the prefix, not from the build tree.
            const QString directory =
                plugin.toObject().value("directory").toString();
            QVERIFY2(QFileInfo(directory).canonicalFilePath().startsWith(
                         installed + u'/'),
                     qPrintable(directory));
        }
        ids.sort();
        QCOMPARE(ids, (QStringList{"claude", "sessions", "terminal"}));

        // The adapter is started from its place in the plugin directory (on
        // macOS a symlink to the one in Contents/MacOS), runs the fake claude.
        QStringList sessions;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                sessions.clear();
                for (const QJsonValue session :
                     call("sessions.list").value("sessions").toArray()) {
                    sessions.append(session.toObject().value("id").toString());
                }
                sessions.sort();
                return sessions == FixtureSessionIds();
            }(),
            TIMEOUT_MS);
        QVERIFY(!sessions.isEmpty());

        // The installed window, against this daemon: it loads the sessions
        // QML module from the install (on macOS through the symlink to
        // Contents/PlugIns) and shows the sessions. Exit 0 or the reason.
        QProcessEnvironment windowEnvironment = environment;
        windowEnvironment.insert("QT_QPA_PLATFORM", "offscreen");
        windowEnvironment.insert("QML_DISABLE_DISK_CACHE", "1");
        // A window that lost the daemon would read the login environment
        // (no tty on stdin): never from the shell of the developer.
        windowEnvironment.insert("SHELL", "/nonexistent/shell");
        QProcess window;
        window.setProcessEnvironment(windowEnvironment);
        window.start(BinDir() + "/roost", {"--smoke-sessions"});
        if (!window.waitForFinished(WINDOW_TIMEOUT_MS)) {
            window.kill();
            window.waitForFinished(TIMEOUT_MS);
            QFAIL("the installed roost --smoke-sessions did not end");
        }
        QCOMPARE(window.exitStatus(), QProcess::NormalExit);
        QVERIFY2(window.exitCode() == 0,
                 qPrintable(QString::fromUtf8(window.readAllStandardError())));
    }
};

QTEST_GUILESS_MAIN(InstallLayoutTest)
#include "install_layout_test.moc"
