#pragma once

#include <QString>

#include <expected>

namespace roost {

// Moves the file at `path` to `<path>.1`, replacing an older one. Nothing
// happens if there is no file.
std::expected<void, QString> RotateLog(const QString& path);

// Rotates `path`, then points stdout and stderr (so that the stderr of child
// processes too) at a new file there, mode 0600. For the process that holds the
// socket lock only: a roostd that lost it must not touch the log of the one
// that won.
std::expected<void, QString> RedirectToLog(const QString& path);

}  // namespace roost
