#pragma once

#include <string>

namespace roost {

// The roost version and the version of the libghostty-vt it is linked with,
// as `--version` prints them: "0.1.0 (libghostty-vt 1.0.1)". The library
// version is read from the library itself, not from its headers.
std::string VersionString();

// The version of the linked libghostty-vt, read from the library: "1.0.1".
std::string GhosttyVtVersion();

}  // namespace roost
