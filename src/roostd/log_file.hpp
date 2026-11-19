#pragma once

#include <QString>

#include <expected>

namespace roost {

// Moves the file at `path` to `<path>.1`, replacing an older one. Nothing
// happens if there is no file.
std::expected<void, QString> RotateLog(const QString& path);

// Makes sure descriptors 0, 1 and 2 are open, pointing the closed ones at
// /dev/null. Call before anything else opens a descriptor: Qt's own pipes would
// take the free low numbers, and RedirectToLog would then replace them.
void OpenClosedStdio();

// Creates `<path>.new` (mode 0600), and only if that works moves `path` to
// `<path>.1` and the new file to `path`, then points stdout and stderr (so that
// the stderr of child processes too) at it. A failure leaves `path` and
// `<path>.1` as they were. For the process that holds the socket lock only: a
// roostd that lost it must not touch the log of the one that won.
std::expected<void, QString> RedirectToLog(const QString& path);

}  // namespace roost
