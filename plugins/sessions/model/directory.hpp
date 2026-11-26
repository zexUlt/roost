#pragma once

#include <QString>

namespace roost::sessions {

// The directory a user typed for a new session, checked: `Path` is the
// expansion of `~` and `~/…` against the home directory and `Problem` the
// reason it cannot be the directory of a session, empty when it can.
struct DirectoryCheck {
    QString Path;
    QString Problem;
};

// The path must be absolute after the expansion and name an existing
// directory the agent can use (exists, is a directory, readable and
// executable for the user, as the claude adapter requires). The text as typed
// is the path when it names an existing one, else the text without surrounding
// blanks. A stat and access(2) checks run in the calling (GUI) thread on every
// call, so a hung mount blocks the window for that time.
DirectoryCheck CheckDirectory(const QString& typed);

// The path as the input shows it: the home directory as `~`.
QString ShownDirectory(const QString& path);

}  // namespace roost::sessions
