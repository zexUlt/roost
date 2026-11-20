// The bug report zip: its entries, contents and names, checked with the
// system's unzip. Everything is injected: temporary directories only.

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "roost/bug_report.hpp"

using namespace roost;

namespace {

const QDateTime TIME(QDate(2026, 11, 20), QTime(13, 5, 9));
const QString FOLDER = QStringLiteral("roost-bug-20261120-130509/");

struct Output {
    int Code = -1;
    QByteArray Out;
};

}  // namespace

class BugReportTest : public QObject {
    Q_OBJECT

private:
    static void Write(const QString& path, const QByteArray& data) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(data), data.size());
    }

    static Output Unzip(const QStringList& arguments) {
        QProcess process;
        process.start(QStandardPaths::findExecutable("unzip"), arguments);
        process.waitForFinished(30000);
        return {process.exitCode(), process.readAllStandardOutput()};
    }

    QStringList Entries(const QString& zip) {
        const Output out = Unzip({"-Z1", zip});
        if (out.Code != 0) {
            return {QStringLiteral("<unzip failed>")};
        }
        QStringList names =
            QString::fromUtf8(out.Out).split('\n', Qt::SkipEmptyParts);
        names.sort();
        return names;
    }

    QByteArray Content(const QString& zip, const QString& name) {
        return Unzip({"-p", zip, FOLDER + name}).Out;
    }

    BugReportRequest Request() {
        BugReportRequest request;
        request.Description = "it broke";
        request.About = "roost: test\n";
        request.LogDirectory = Dir_.filePath("run");
        request.ConfigDirectory = Dir_.filePath("config");
        request.OutputDirectory = Dir_.filePath("out");
        request.Time = TIME;
        return request;
    }

    QTemporaryDir Dir_;

private slots:
    void init() {
        QVERIFY(Dir_.isValid());
        QDir(Dir_.path()).removeRecursively();
        QVERIFY(QDir().mkpath(Dir_.path()));
        QVERIFY(QDir().mkpath(Dir_.filePath("run")));
        if (QStandardPaths::findExecutable("unzip").isEmpty()) {
            QSKIP("unzip is not installed");
        }
    }

    // All five logs, compressible, empty and random ones (deflated and
    // stored), the description in UTF-8: unzip -t passes, the entries and the
    // bytes are exactly what went in.
    void TheZipPassesUnzipAndHasExactlyTheEntries() {
        QByteArray compressible;
        for (int i = 0; i < 2000; ++i) {
            compressible +=
                "2026-11-20 roostd: line " + QByteArray::number(i) + "\n";
        }
        QByteArray random(5000, 0);
        for (char& c : random) {
            c = static_cast<char>(QRandomGenerator::global()->bounded(256));
        }
        Write(Dir_.filePath("run/roostd.log"), compressible);
        Write(Dir_.filePath("run/roostd.log.1"), random);
        Write(Dir_.filePath("run/roostd.start.log"), "");
        Write(Dir_.filePath("run/roost.log"), "window\n");
        Write(Dir_.filePath("run/roost.log.1"), "old window\n");
        Write(Dir_.filePath("run/roostd.sock.lock"), "not a log");
        BugReportRequest request = Request();
        request.Description =
            QString::fromUtf8("не работает ✓ 日本語 😀\nвторая строка");
        const auto zip = CreateBugReport(request);
        QVERIFY2(zip.has_value(), qPrintable(zip ? QString() : zip.error()));
        QCOMPARE(*zip, Dir_.filePath("out/roost-bug-20261120-130509.zip"));
        const Output tested = Unzip({"-t", *zip});
        QVERIFY2(tested.Code == 0, tested.Out.constData());
        QStringList expected;
        for (const char* name :
             {"about.txt", "description.txt", "logs/roostd.log",
              "logs/roostd.log.1", "logs/roostd.start.log", "logs/roost.log",
              "logs/roost.log.1"}) {
            expected << FOLDER + name;
        }
        expected.sort();
        QCOMPARE(Entries(*zip), expected);
        QCOMPARE(Content(*zip, "about.txt"), QByteArray("roost: test\n"));
        QCOMPARE(Content(*zip, "description.txt"),
                 request.Description.toUtf8());
        QCOMPARE(Content(*zip, "logs/roostd.log"), compressible);
        QCOMPARE(Content(*zip, "logs/roostd.log.1"), random);
        QCOMPARE(Content(*zip, "logs/roostd.start.log"), QByteArray());
        QCOMPARE(Content(*zip, "logs/roost.log"), QByteArray("window\n"));
        // The compressible one is really deflated (method 8).
        const Output listing = Unzip({"-Zv", *zip});
        QVERIFY(listing.Out.contains("deflated"));
        QVERIFY(listing.Out.contains("stored"));
        // For the record of a run: the listing of the zip.
        qInfo().noquote() << QString::fromUtf8(Unzip({"-l", *zip}).Out);
        // Private: the logs may hold paths and prompts.
        QCOMPARE(QFileInfo(*zip).permissions() &
                     (QFile::ReadGroup | QFile::ReadOther),
                 QFileDevice::Permissions());
    }

    void OnlyTheExistingLogsGoIn() {
        Write(Dir_.filePath("run/roostd.log"), "d\n");
        const auto zip = CreateBugReport(Request());
        QVERIFY2(zip.has_value(), qPrintable(zip ? QString() : zip.error()));
        QCOMPARE(Entries(*zip),
                 (QStringList{FOLDER + "about.txt", FOLDER + "description.txt",
                              FOLDER + "logs/roostd.log"}));
    }

    // The cap is lowered for the test: a log over it goes in as its tail
    // behind the marker, one under it whole.
    void ALogOverTheCapGoesInAsItsTail() {
        Write(Dir_.filePath("run/roostd.log"), "0123456789abcdefghij");
        Write(Dir_.filePath("run/roost.log"), "short\n");
        BugReportRequest request = Request();
        request.MaxLogBytes = 8;
        const QByteArray marker = "[roost: first 12 bytes cut]\n";
        const QList<BugReportFile> listed =
            LogFiles(request.LogDirectory, request.MaxLogBytes);
        QCOMPARE(listed.size(), 2);
        QCOMPARE(listed[0].Name, QString("logs/roostd.log"));
        QCOMPARE(listed[0].Size, 8 + marker.size());
        QCOMPARE(listed[1].Size, 6);
        const auto zip = CreateBugReport(request);
        QVERIFY2(zip.has_value(), qPrintable(zip ? QString() : zip.error()));
        QCOMPARE(Content(*zip, "logs/roostd.log"),
                 marker + QByteArray("cdefghij"));
        QCOMPARE(Content(*zip, "logs/roost.log"), QByteArray("short\n"));
        // Exactly the cap is not cut.
        request.MaxLogBytes = 20;
        const auto whole = CreateBugReport(request);
        QVERIFY(whole.has_value());
        QCOMPARE(Content(*whole, "logs/roostd.log"),
                 QByteArray("0123456789abcdefghij"));
    }

    void NoLogsIsStillAReport() {
        const auto zip = CreateBugReport(Request());
        QVERIFY2(zip.has_value(), qPrintable(zip ? QString() : zip.error()));
        QCOMPARE(Entries(*zip), (QStringList{FOLDER + "about.txt",
                                             FOLDER + "description.txt"}));
        QCOMPARE(Unzip({"-t", *zip}).Code, 0);
    }

    // The store's own files only: <id>.json and keymap.json, not plugins/, not
    // another file, not a name that is no plugin id.
    void SettingsGoInOnlyWithTheFlag() {
        Write(Dir_.filePath("config/keymap.json"), "{\"k\":1}");
        Write(Dir_.filePath("config/sessions.json"), "{\"s\":2}");
        Write(Dir_.filePath("config/notes.txt"), "no");
        Write(Dir_.filePath("config/Bad Id.json"), "no");
        Write(Dir_.filePath("config/plugins/mine/plugin.json"), "no");
        Write(Dir_.filePath("config/sub/x.json"), "no");
        BugReportRequest request = Request();
        auto zip = CreateBugReport(request);
        QVERIFY2(zip.has_value(), qPrintable(zip ? QString() : zip.error()));
        QCOMPARE(Entries(*zip), (QStringList{FOLDER + "about.txt",
                                             FOLDER + "description.txt"}));

        request.IncludeSettings = true;
        zip = CreateBugReport(request);
        QVERIFY2(zip.has_value(), qPrintable(zip ? QString() : zip.error()));
        QCOMPARE(Entries(*zip),
                 (QStringList{FOLDER + "about.txt", FOLDER + "description.txt",
                              FOLDER + "settings/keymap.json",
                              FOLDER + "settings/sessions.json"}));
        QCOMPARE(Content(*zip, "settings/sessions.json"),
                 QByteArray("{\"s\":2}"));
    }

    void ASettingsFlagWithoutAConfigDirectoryIsFine() {
        BugReportRequest request = Request();
        request.IncludeSettings = true;
        request.ConfigDirectory.clear();
        QVERIFY(CreateBugReport(request).has_value());
    }

    void AnExistingZipIsNeverOverwritten() {
        const BugReportRequest request = Request();
        const auto first = CreateBugReport(request);
        QVERIFY(first.has_value());
        const QByteArray before = [&] {
            QFile f(*first);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        }();
        BugReportRequest other = request;
        other.Description = "second";
        const auto second = CreateBugReport(other);
        const auto third = CreateBugReport(other);
        QVERIFY(second.has_value() && third.has_value());
        QCOMPARE(*second, Dir_.filePath("out/roost-bug-20261120-130509-2.zip"));
        QCOMPARE(*third, Dir_.filePath("out/roost-bug-20261120-130509-3.zip"));
        QFile f(*first);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), before);
        // The folder inside is the same; the description is the second's.
        QCOMPARE(Content(*second, "description.txt"), QByteArray("second"));
        QCOMPARE(QDir(Dir_.filePath("out"))
                     .entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                     .size(),
                 3);
    }

    // A directory that cannot be made, one that cannot be written to, a log
    // that cannot be read: an error, and no file (no .part either).
    void AFailureLeavesNoFile() {
        Write(Dir_.filePath("blocker"), "a file, not a directory");
        BugReportRequest request = Request();
        request.OutputDirectory = Dir_.filePath("blocker/out");
        QVERIFY(!CreateBugReport(request).has_value());

        QVERIFY(QDir().mkpath(Dir_.filePath("ro")));
        QVERIFY(QFile::setPermissions(Dir_.filePath("ro"),
                                      QFile::ReadOwner | QFile::ExeOwner));
        request.OutputDirectory = Dir_.filePath("ro");
        if (QFileInfo(Dir_.filePath("ro")).isWritable()) {
            QFile::setPermissions(Dir_.filePath("ro"), QFile::ReadOwner |
                                                           QFile::WriteOwner |
                                                           QFile::ExeOwner);
            QSKIP("the process writes to a 0500 directory (root?)");
        }
        const auto result = CreateBugReport(request);
        QVERIFY(!result.has_value());
        QVERIFY(!result.error().isEmpty());
        QFile::setPermissions(Dir_.filePath("ro"), QFile::ReadOwner |
                                                       QFile::WriteOwner |
                                                       QFile::ExeOwner);
        QVERIFY(QDir(Dir_.filePath("ro"))
                    .entryList(QDir::AllEntries | QDir::NoDotAndDotDot |
                               QDir::Hidden)
                    .isEmpty());

        Write(Dir_.filePath("run/roostd.log"), "x");
        QVERIFY(QFile::setPermissions(Dir_.filePath("run/roostd.log"),
                                      QFileDevice::Permissions()));
        if (QFile(Dir_.filePath("run/roostd.log")).open(QIODevice::ReadOnly)) {
            QSKIP("the process reads a 000 file (root?)");
        }
        request.OutputDirectory = Dir_.filePath("out");
        QVERIFY(!CreateBugReport(request).has_value());
        QVERIFY(!QDir(Dir_.filePath("out")).exists() ||
                QDir(Dir_.filePath("out"))
                    .entryList(QDir::AllEntries | QDir::NoDotAndDotDot |
                               QDir::Hidden)
                    .isEmpty());
        QFile::setPermissions(Dir_.filePath("run/roostd.log"),
                              QFile::ReadOwner | QFile::WriteOwner);
    }

    void TheIssueUrlStaysShort() {
        QString text;
        for (int i = 0; i < 3000; ++i) {
            text += QString::fromUtf8("жук 😀 & ");
        }
        for (const QString& description : {QString(), QString("short"), text}) {
            const QString url = IssueUrl(description, "roost-bug-x.zip");
            QVERIFY(url.size() <= MAX_ISSUE_URL_LENGTH);
            QVERIFY(url.startsWith(
                "https://github.com/zexUlt/roost/issues/new?title=&body="));
            QVERIFY(url.contains("roost-bug-x.zip"));
        }
        QVERIFY(IssueUrl("short", {}).contains("short"));
        QVERIFY(IssueUrl(text, {}).size() > 5000);
    }

    void TheAboutTextNamesTheVersions() {
        const QString about = AboutText("/tmp/x/roostd.sock", "Metal");
        for (const char* key :
             {"roost:", "Qt (runtime):", "OS:", "Kernel:", "CPU architecture:",
              "QPA platform:", "Graphics API: Metal",
              "Socket: /tmp/x/roostd.sock"}) {
            QVERIFY2(about.contains(key), key);
        }
        QVERIFY(!AboutText("/s", {}).contains("Graphics API"));
    }
};

QTEST_GUILESS_MAIN(BugReportTest)
#include "bug_report_test.moc"
