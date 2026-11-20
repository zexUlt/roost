#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include <expected>

namespace roost {

// A bug report: a zip with the logs of the window and of roostd, an about.txt
// with versions and the platform, the user's description and, only if asked
// for, the settings. Plain C++ and QtCore; the QML side is BugReport
// (bug_report_controller.hpp).

// A file that goes into the zip: its name in the zip under the folder of the
// report, its path on disk and its size.
struct BugReportFile {
    QString Name;
    QString Path;
    qint64 Size = 0;
};

struct BugReportRequest {
    // The user's text, written to description.txt as UTF-8.
    QString Description;
    // The text of about.txt; AboutText() makes it.
    QString About;
    // Where roostd.log and roost.log live: the directory of the socket.
    QString LogDirectory;
    // The directory of the store (ConfigDirectory()); read only with
    // IncludeSettings.
    QString ConfigDirectory;
    bool IncludeSettings = false;
    // Where the zip goes; created if missing. See DefaultOutputDirectory().
    QString OutputDirectory;
    // Names the report; local time.
    QDateTime Time = QDateTime::currentDateTime();
};

// The logs that exist in `logDirectory`: roostd.log, roostd.log.1,
// roostd.start.log, roost.log, roost.log.1, as "logs/<name>".
QList<BugReportFile> LogFiles(const QString& logDirectory);

// The store's own files in `configDirectory`: "<plugin id>.json" with a valid
// id (the keymap included), as "settings/<name>". Nothing else of the
// directory, plugins/ included.
QList<BugReportFile> SettingsFiles(const QString& configDirectory);

// Versions, platform and the socket of this process. `graphicsApi` is the
// graphics API of the window, or empty if there is none.
QString AboutText(const QString& socketPath, const QString& graphicsApi);

// The Downloads directory of the user, else the home directory.
QString DefaultOutputDirectory();

// "roost-bug-<YYYYMMDD-HHMMSS>", the folder of the report in the zip and the
// base of the zip's name.
QString ReportName(const QDateTime& time);

// The URL of a new GitHub issue with an empty title and `description` as the
// body, cut so that the URL stays under MAX_ISSUE_URL_LENGTH. `zipName`, if
// not empty, goes into a line that asks to attach the zip.
constexpr qsizetype MAX_ISSUE_URL_LENGTH = 6000;
QString IssueUrl(const QString& description, const QString& zipName);

// Writes the zip and returns its path. The zip is written under a temporary
// name and renamed at the end, so a failure leaves no file in the directory;
// an existing zip is never overwritten (-2, -3 follow). Fails if an entry or
// the archive would pass 4 GiB: there is no zip64.
std::expected<QString, QString> CreateBugReport(
    const BugReportRequest& request);

}  // namespace roost
