// The directory typed for a new session (sessions/model/directory.hpp):
// CheckDirectory says whether it can be the directory of a session and why
// not, and ShownDirectory shows the home directory as `~`. The home
// directory and the tree are made by the test, on the real file system.

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <optional>

#include <unistd.h>

#include "helpers/scoped_env.hpp"
#include "sessions/model/directory.hpp"

using namespace roost::sessions;
using roost::testing::ScopedEnv;

class SessionsDirectoryTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> Dir_;
    std::unique_ptr<ScopedEnv> Home_;

    QString Path(const QString& relative) const {
        return Dir_->filePath(relative);
    }

    void VerifyGood(const QString& typed, const QString& path) {
        const DirectoryCheck check = CheckDirectory(typed);
        QCOMPARE(check.Problem, QString());
        QCOMPARE(check.Path, path);
    }

    void VerifyRefused(const QString& typed, const QString& path,
                       const QString& problem) {
        const DirectoryCheck check = CheckDirectory(typed);
        QCOMPARE(check.Problem, problem);
        QCOMPARE(check.Path, path);
    }

private slots:
    // The tree: home/ (the home directory), home/notes, home/file.txt,
    // outside/, and the symlinks and the directories with modes.
    void init() {
        Dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(Dir_->isValid());
        QVERIFY(QDir().mkpath(Path("home/notes")));
        QVERIFY(QDir().mkpath(Path("outside")));
        QSaveFile file(Path("home/file.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly) && file.commit());
        Home_ = std::make_unique<ScopedEnv>("HOME", Path("home").toUtf8());
        QCOMPARE(QDir::homePath(), Path("home"));
    }

    void cleanup() {
        // Modes back, so that the directory can be removed.
        for (const char* name : {"locked", "unreadable"}) {
            QFile::setPermissions(Path(name), QFile::ReadOwner |
                                                  QFile::WriteOwner |
                                                  QFile::ExeOwner);
        }
        Home_.reset();
        Dir_.reset();
    }

    void NothingTypedIsNoDirectory() {
        VerifyRefused("", "", "No directory");
        VerifyRefused("   ", "", "No directory");
        VerifyRefused(" \t ", "", "No directory");
    }

    void AnExistingAbsoluteDirectoryIsGood() {
        VerifyGood(Path("outside"), Path("outside"));
        VerifyGood("/", "/");
    }

    void TheHomeDirectoryIsTheTilde() {
        VerifyGood("~", Path("home"));
        VerifyGood("~/notes", Path("home/notes"));
        VerifyRefused("~/missing", Path("home/missing"),
                      Path("home/missing") + " does not exist");
        VerifyRefused("~/file.txt", Path("home/file.txt"),
                      Path("home/file.txt") + " is not a directory");
    }

    // Only `~` and `~/…` are expanded: the home of another user is not
    // looked up, and a tilde inside a name is a name.
    void ATildeOfAnotherUserOrInsideANameIsNotExpanded() {
        VerifyRefused("~nobody", "~nobody", "~nobody is not an absolute path");
        VerifyRefused("~nobody/app", "~nobody/app",
                      "~nobody/app is not an absolute path");
        VerifyRefused("~notes", "~notes", "~notes is not an absolute path");
    }

    void ARelativePathIsRefused() {
        VerifyRefused("notes", "notes", "notes is not an absolute path");
        VerifyRefused("./notes", "./notes", "./notes is not an absolute path");
        VerifyRefused("../outside", "../outside",
                      "../outside is not an absolute path");
    }

    void AMissingPathOrAFileIsRefused() {
        VerifyRefused(Path("missing"), Path("missing"),
                      Path("missing") + " does not exist");
        VerifyRefused(Path("home/file.txt"), Path("home/file.txt"),
                      Path("home/file.txt") + " is not a directory");
    }

    void ASymlinkIsFollowed() {
        QVERIFY(QFile::link(Path("outside"), Path("to-outside")));
        QVERIFY(QFile::link(Path("home/file.txt"), Path("to-file")));
        QVERIFY(QFile::link(Path("missing"), Path("broken")));
        // The path stays as typed: the link is not resolved.
        VerifyGood(Path("to-outside"), Path("to-outside"));
        VerifyRefused(Path("to-file"), Path("to-file"),
                      Path("to-file") + " is not a directory");
        VerifyRefused(Path("broken"), Path("broken"),
                      Path("broken") + " does not exist");
    }

    // root passes any mode.
    void ADirectoryTheUserCannotReadOrEnterIsRefused() {
        if (geteuid() == 0) {
            QSKIP("root enters and reads any directory");
        }
        QVERIFY(QDir().mkpath(Path("locked")));
        QVERIFY(QDir().mkpath(Path("unreadable")));
        // Write and enter, no read.
        QVERIFY(QFile::setPermissions(Path("unreadable"),
                                      QFile::WriteOwner | QFile::ExeOwner));
        VerifyRefused(Path("unreadable"), Path("unreadable"),
                      Path("unreadable") + " cannot be read");
        // Read and write, no enter.
        QVERIFY(QFile::setPermissions(Path("locked"),
                                      QFile::ReadOwner | QFile::WriteOwner));
        VerifyRefused(Path("locked"), Path("locked"),
                      Path("locked") + " cannot be entered");
    }

    // The text as typed is the path when it names an existing one, else the
    // text without the blanks at its ends.
    void BlanksAtTheEndsAreTrimmedUnlessThePathAsTypedExists() {
        QVERIFY(QDir().mkpath(Path("blank ")));
        // "outside" exists, "outside " does not: trimmed.
        VerifyGood(Path("outside") + " ", Path("outside"));
        VerifyGood("  " + Path("outside") + "  ", Path("outside"));
        VerifyGood("  ~/notes ", Path("home/notes"));
        // "blank " exists as typed, "blank" does not.
        VerifyGood(Path("blank "), Path("blank "));
        VerifyRefused(Path("blank"), Path("blank"),
                      Path("blank") + " does not exist");
        // A text with a blank that names nothing says so as trimmed.
        VerifyRefused(Path("missing") + " ", Path("missing"),
                      Path("missing") + " does not exist");
    }

    void TheHomeDirectoryIsShownAsTheTilde() {
        QCOMPARE(ShownDirectory(Path("home")), QString("~"));
        QCOMPARE(ShownDirectory(Path("home/notes")), QString("~/notes"));
        QCOMPARE(ShownDirectory(Path("home/notes/a b")),
                 QString("~/notes/a b"));
        QCOMPARE(ShownDirectory(Path("outside")), Path("outside"));
        // What is shown is read back as the same directory.
        VerifyGood(ShownDirectory(Path("home/notes")), Path("home/notes"));
    }

    // A directory whose name starts with the name of the home directory is
    // not under it.
    void ASiblingWithTheNameOfTheHomeAsAPrefixIsNotUnderIt() {
        const QString sibling = Path("home") + "x";
        QCOMPARE(ShownDirectory(sibling), sibling);
        QCOMPARE(ShownDirectory(sibling + "/notes"), sibling + "/notes");
        QCOMPARE(ShownDirectory(Path("home") + "-old"), Path("home") + "-old");
    }

    // A root home directory would turn every path into `~/…`.
    void ARootHomeDirectoryShowsNoTilde() {
        const ScopedEnv root("HOME", QByteArray("/"));
        QCOMPARE(QDir::homePath(), QString("/"));
        QCOMPARE(ShownDirectory("/usr"), QString("/usr"));
        QCOMPARE(ShownDirectory("/"), QString("/"));
    }
};

QTEST_MAIN(SessionsDirectoryTest)
#include "sessions_directory_test.moc"
