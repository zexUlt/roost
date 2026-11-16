# roost

A desktop app for macOS and Linux that finds every background coding-agent
session, groups them, and shows each agent's own TUI in a terminal panel, all
driven from the keyboard. Claude Code first; Codex and other agents through the
same adapter interface. Built on Qt Quick and C++, with libghostty-vt as the
terminal emulator. Every feature is a plugin, and panels can be linked to each
other.

Status: skeleton. Both binaries build and start, nothing more yet; see
`docs/design.md` for the design and the plan.

## Build

Needs Clang 17+ (Apple clang on macOS), CMake 3.25+, Ninja, Qt 6.8+ and
Zig 0.17+. On macOS: `brew install cmake ninja qt zig`.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Binaries land in `build/dev/bin/`. Other presets: `asan-ubsan`, `tsan`, `release`.

## License

Apache-2.0, see `LICENSE`. Third-party components and their licences are listed
in `THIRD_PARTY_LICENSES`.
