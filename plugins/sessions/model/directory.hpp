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
// directory the user can enter (exists, is a directory, executable for the
// user). Surrounding blanks are not part of the path. One stat and one
// access(2), so a slow mount costs one round trip.
DirectoryCheck CheckDirectory(const QString& typed);

// The path as the input shows it: the home directory as `~`.
QString ShownDirectory(const QString& path);

}  // namespace roost::sessions
