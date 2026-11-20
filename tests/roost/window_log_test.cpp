// roost.log, the window's own log: the lines Qt messages become, the previous
// handler still getting them, rotation at start, an unopenable file, writers
// that append at once, and what a TerminalItem logs when the session changes.
// Temporary directories only: never the real log directory of the user.

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>

#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <thread>
#include <vector>

#include "qml_api/terminal_item.hpp"
#include "roost/window_log.hpp"
#include "terminal/terminal_session.hpp"

using namespace roost;

namespace {

Q_LOGGING_CATEGORY(TestCategory, "roost.test", QtDebugMsg)

constexpr qint64 BIG_LIMIT = 1024 * 1024;
constexpr int LINES_PER_WRITER = 500;

QStringList PREVIOUS_MESSAGES;

void CollectingHandler(QtMsgType, const QMessageLogContext&,
                       const QString& message) {
    PREVIOUS_MESSAGES.append(message);
}

QByteArray Read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The permission bits of a file, from stat: QFile adds the user bits itself.
mode_t Mode(const QString& path) {
    struct stat info{};
    return ::stat(QFile::encodeName(path).constData(), &info) == 0
               ? (info.st_mode & 0777)
               : mode_t(-1);
}

QStringList Lines(const QString& path) {
    return QString::fromUtf8(Read(path)).split(u'\n', Qt::SkipEmptyParts);
}

std::shared_ptr<TerminalSession> NewSession() {
    auto session = TerminalSession::Create(20, 8);
    if (!session) {
        qFatal("%s", qPrintable(session.error()));
    }
    return std::shared_ptr<TerminalSession>(std::move(*session));
}

}  // namespace

class WindowLogTest : public QObject {
    Q_OBJECT

private slots:
    void init() {
        PREVIOUS_MESSAGES.clear();
        // What a test starts from: a handler of its own, as the default one
        // would print to the console.
        qInstallMessageHandler(&CollectingHandler);
    }

    void cleanup() {
        UninstallWindowLog();
        qInstallMessageHandler(nullptr);
    }

    void PathIsNextToTheSocket() {
        QCOMPARE(WindowLogPath("/run/user/1/roost/roostd.sock"),
                 QStringLiteral("/run/user/1/roost/roost.log"));
    }

    void MessageLandsInTheFileAndReachesThePreviousHandler() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("roost.log");
        QVERIFY(InstallWindowLog(path));

        qCWarning(TestCategory, "something %d", 42);
        qInfo("plain info");

        const QStringList lines = Lines(path);
        QCOMPARE(lines.size(), 2);
        const QString pid = QString::number(QCoreApplication::applicationPid());
        const QRegularExpression categorized(
            QStringLiteral(R"(^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3} pid=%1 )"
                           R"(W roost\.test: something 42$)")
                .arg(pid));
        QVERIFY2(categorized.match(lines[0]).hasMatch(), qPrintable(lines[0]));
        // The "default" category is left out.
        const QRegularExpression plain(
            QStringLiteral(R"(^\S+ pid=%1 I plain info$)").arg(pid));
        QVERIFY2(plain.match(lines[1]).hasMatch(), qPrintable(lines[1]));

        QCOMPARE(PREVIOUS_MESSAGES,
                 (QStringList{"something 42", "plain info"}));
        QCOMPARE(Mode(path), mode_t(0600));
    }

    void NewlineInTheMessageStaysOneLineInTheFile() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("roost.log");
        QVERIFY(InstallWindowLog(path));

        const QString message = QStringLiteral("a\nb\r\nc\\d");
        qInfo("%s", qPrintable(message));

        const QStringList lines = Lines(path);
        QCOMPARE(lines.size(), 1);
        QVERIFY2(lines[0].endsWith(QStringLiteral("I a\\nb\\r\\nc\\\\d")),
                 qPrintable(lines[0]));
        QCOMPARE(Read(path).count('\n'), 1);
        QCOMPARE(PREVIOUS_MESSAGES, QStringList{message});
    }

    void SymlinkAtTheLogPathIsNotFollowed() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = dir.filePath("target");
        QFile file(target);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("untouched\n") > 0);
        file.close();
        const QString path = dir.filePath("roost.log");
        QVERIFY(::symlink(QFile::encodeName(target).constData(),
                          QFile::encodeName(path).constData()) == 0);

        QVERIFY(!WindowLogFile::Open(path));
        QVERIFY(!InstallWindowLog(path));
        QCOMPARE(PREVIOUS_MESSAGES.size(), 1);
        QVERIFY(PREVIOUS_MESSAGES[0].contains("no window log"));

        qWarning("still forwarded");
        QCOMPARE(PREVIOUS_MESSAGES.size(), 2);
        QCOMPARE(PREVIOUS_MESSAGES[1], QStringLiteral("still forwarded"));
        QCOMPARE(Read(target), QByteArray("untouched\n"));
    }

    void LevelLetters() {
        QMessageLogContext context("file", 1, "fn", "default");
        const QDateTime time = QDateTime::currentDateTime();
        const QList<QPair<QtMsgType, char>> levels{{QtDebugMsg, 'D'},
                                                   {QtInfoMsg, 'I'},
                                                   {QtWarningMsg, 'W'},
                                                   {QtCriticalMsg, 'C'},
                                                   {QtFatalMsg, 'F'}};
        for (const auto& [type, letter] : levels) {
            const QByteArray line =
                FormatWindowLogLine(type, context, "m", time, 7);
            QVERIFY2(line.contains(QByteArray(" pid=7 ") + letter + " m\n"),
                     line.constData());
        }
    }

    void LargeFileIsRotatedAtStart() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("roost.log");
        QFile old(path);
        QVERIFY(old.open(QIODevice::WriteOnly));
        QVERIFY(old.write(QByteArray(100, 'x') + '\n') > 0);
        old.close();

        QVERIFY(InstallWindowLog(path, /*rotateAbove=*/50));
        qInfo("after rotation");

        QCOMPARE(Read(path + ".1"), QByteArray(100, 'x') + '\n');
        const QStringList lines = Lines(path);
        QCOMPARE(lines.size(), 1);
        QVERIFY(lines[0].endsWith("I after rotation"));
    }

    void SmallFileIsKeptAndAppendedTo() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("roost.log");
        QFile old(path);
        QVERIFY(old.open(QIODevice::WriteOnly));
        QVERIFY(old.write("earlier\n") > 0);
        old.close();

        QVERIFY(InstallWindowLog(path, BIG_LIMIT));
        qInfo("later");

        QVERIFY(!QFile::exists(path + ".1"));
        const QStringList lines = Lines(path);
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines[0], QStringLiteral("earlier"));
        QVERIFY(lines[1].endsWith("I later"));
    }

    void UnopenablePathDoesNotStopForwarding() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // A file where the directory should be.
        QFile blocker(dir.filePath("blocker"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        const QString path = dir.filePath("blocker/roost.log");

        QVERIFY(!WindowLogFile::Open(path));
        QVERIFY(!InstallWindowLog(path));
        // The one warning of the failure went to the handler that was there.
        QCOMPARE(PREVIOUS_MESSAGES.size(), 1);
        QVERIFY(PREVIOUS_MESSAGES[0].contains("no window log"));

        qWarning("still forwarded");
        QCOMPARE(PREVIOUS_MESSAGES.size(), 2);
        QCOMPARE(PREVIOUS_MESSAGES[1], QStringLiteral("still forwarded"));
    }

    void MissingDirectoryIsMade() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString directory = dir.filePath("roost-501");
        QVERIFY(InstallWindowLog(directory + "/roost.log"));
        qInfo("first");
        QCOMPARE(Lines(directory + "/roost.log").size(), 1);
        QCOMPARE(Mode(directory), mode_t(0700));
    }

    void TwoWritersLoseNoLines() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("roost.log");
        // Two descriptions of the file, as two roost processes have.
        auto first = WindowLogFile::Open(path, BIG_LIMIT);
        auto second = WindowLogFile::Open(path, BIG_LIMIT);
        QVERIFY(first && second);

        const auto work = [](const WindowLogFile& file, char writer) {
            for (int i = 0; i < LINES_PER_WRITER; ++i) {
                file.Write(QByteArray(1, writer) + QByteArray::number(i) + ' ' +
                           QByteArray(200, writer) + '\n');
            }
        };
        std::thread a(work, std::cref(*first), 'a');
        std::thread b(work, std::cref(*second), 'b');
        a.join();
        b.join();

        const QStringList lines = Lines(path);
        QCOMPARE(lines.size(), 2 * LINES_PER_WRITER);
        int fromA = 0;
        int fromB = 0;
        for (const QString& line : lines) {
            const QChar writer = line[0];
            QVERIFY2(writer == u'a' || writer == u'b', qPrintable(line));
            // Whole: the writer's letter after the number, 200 times.
            QVERIFY2(line.endsWith(QString(200, writer)), qPrintable(line));
            (writer == u'a' ? fromA : fromB)++;
        }
        QCOMPARE(fromA, LINES_PER_WRITER);
        QCOMPARE(fromB, LINES_PER_WRITER);
    }

    void StartupLineNamesTheEnvironment() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("roost.log");
        QVERIFY(InstallWindowLog(path));
        LogWindowStartup();
        const QStringList lines = Lines(path);
        QCOMPARE(lines.size(), 1);
        QVERIFY2(lines[0].contains(" I roost.window: roost "),
                 qPrintable(lines[0]));
        QVERIFY2(lines[0].contains(QStringLiteral(", Qt ") +
                                   QString::fromLatin1(qVersion())),
                 qPrintable(lines[0]));
        QVERIFY2(lines[0].contains("platform offscreen"), qPrintable(lines[0]));
    }

    // The log of a switch: the order of setSession and status, with ids, and
    // nothing the terminal shows.
    void SessionSwitchIsLoggedWithoutTerminalOutput() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("roost.log");
        QVERIFY(InstallWindowLog(path));

        TerminalItem item;
        const auto a = NewSession();
        const auto b = NewSession();
        {
            const auto lock = a->Lock();
            a->Write(lock, "SECRET-OUTPUT");
        }
        // No engine: the item cannot open a pane, and says why.
        item.setSessionRef(QVariantMap{{"agent", "claude"}, {"id", "s1"}});
        item.setSession(a);
        item.setSessionRef(QVariantMap{{"agent", "claude"}, {"id", "s2"}});
        item.setSession(b);
        item.setSession(nullptr);

        const QString log = QString::fromUtf8(Read(path));
        const QStringList lines = Lines(path);
        QVERIFY2(!log.contains("SECRET"), qPrintable(log));
        QStringList terminal;
        for (const QString& line : lines) {
            if (line.contains(" roost.terminal: ")) {
                terminal.append(line.section(" roost.terminal: ", 1));
            }
        }
        QCOMPARE(terminal,
                 (QStringList{
                     "status none -> failed, message \"no roostd API in this "
                     "window\"",
                     "setSession none -> claude/s1",
                     "setSession claude/s1 -> claude/s2",
                     "setSession claude/s2 -> none",
                 }));
    }
};

int main(int argc, char** argv) {
    // The startup line names the platform: do not depend on ctest's
    // environment.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    WindowLogTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "window_log_test.moc"
