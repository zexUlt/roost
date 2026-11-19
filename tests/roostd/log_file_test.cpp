#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>

#include "roostd/log_file.hpp"

using namespace roost;

namespace {

bool Write(const QString& path, const QByteArray& content) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) > 0;
}

QByteArray Read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

}  // namespace

class LogFileTest : public QObject {
    Q_OBJECT

private slots:
    void RotateMovesTheFileAside() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString log = dir.filePath("roostd.log");
        QVERIFY(Write(log, "run 2\n"));
        QVERIFY(RotateLog(log));
        QVERIFY(!QFile::exists(log));
        QCOMPARE(Read(log + ".1"), QByteArray("run 2\n"));
    }

    void RotateReplacesAnOlderCopy() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString log = dir.filePath("roostd.log");
        QVERIFY(Write(log + ".1", "run 1\n"));
        QVERIFY(Write(log, "run 2\n"));
        QVERIFY(RotateLog(log));
        QCOMPARE(Read(log + ".1"), QByteArray("run 2\n"));
        QVERIFY(!QFile::exists(log));
    }

    void RotateWithoutAFileDoesNothing() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString log = dir.filePath("roostd.log");
        QVERIFY(RotateLog(log));
        QVERIFY(!QFile::exists(log));
        QVERIFY(!QFile::exists(log + ".1"));
    }

    void RotateReportsAFailure() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // A directory cannot take the place of a file.
        const QString log = dir.filePath("roostd.log");
        QVERIFY(Write(log, "x"));
        QVERIFY(QDir().mkpath(log + ".1"));
        const auto rotated = RotateLog(log);
        QVERIFY(!rotated);
        QVERIFY(rotated.error().contains("roostd.log"));
        QCOMPARE(Read(log), QByteArray("x"));
    }

    // Failed opens return before any dup2, so the test's own stdio is safe.
    void RedirectInAMissingDirectoryFails() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto redirected = RedirectToLog(dir.filePath("none/roostd.log"));
        QVERIFY(!redirected);
        QVERIFY(redirected.error().contains("cannot open"));
        QVERIFY(!QFile::exists(dir.filePath("none")));
    }

    void FailedOpenLeavesTheOldLogsAlone() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString log = dir.filePath("roostd.log");
        QVERIFY(Write(log, "current\n"));
        QVERIFY(Write(log + ".1", "older\n"));
        // A directory where the new file goes: open fails.
        QVERIFY(QDir().mkpath(log + ".new"));
        const auto redirected = RedirectToLog(log);
        QVERIFY(!redirected);
        QVERIFY(redirected.error().contains("cannot open"));
        QCOMPARE(Read(log), QByteArray("current\n"));
        QCOMPARE(Read(log + ".1"), QByteArray("older\n"));
    }
};

QTEST_GUILESS_MAIN(LogFileTest)
#include "log_file_test.moc"
