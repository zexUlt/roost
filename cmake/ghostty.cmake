# libghostty-vt from source (design.md §3, §11). Its CMake wrapper runs
# `zig build -Demit-lib-vt` and exports the imported targets ghostty-vt and
# ghostty-vt-static. roost links the static one.
#
# Fetched by tag, which is a shallow clone; a commit hash would need the whole
# history. When written, vt-1.0.1 pointed at 108cacb17ed8990840cd14abfce43d8ed8503ca1.
# tests/ checks that the linked library reports this version.
# To build against a local checkout instead:
#   cmake --preset dev -DFETCHCONTENT_SOURCE_DIR_GHOSTTY=/path/to/ghostty
#
# The debug presets build the library with -Doptimize=ReleaseSafe
# (GHOSTTY_ZIG_BUILD_FLAGS): Zig's own Debug mode checks every page after each
# change and records a stack trace per allocation, so feeding a terminal a few
# hundred kilobytes takes seconds. ReleaseSafe keeps the bounds and overflow
# checks.

set(ROOST_GHOSTTY_VT_VERSION 1.0.1)
set(ROOST_ZIG_MIN_VERSION 0.17.0)

# The wrapper would fail on a missing zig with a bare "could not find
# ZIG_EXECUTABLE"; say what to install instead.
find_program(ZIG_EXECUTABLE zig)
if(NOT ZIG_EXECUTABLE)
    message(FATAL_ERROR
        "zig is not in PATH. libghostty-vt is built with Zig ${ROOST_ZIG_MIN_VERSION} "
        "or newer: `brew install zig` on macOS, https://ziglang.org/download/ on Linux.")
endif()

execute_process(
    COMMAND "${ZIG_EXECUTABLE}" version
    OUTPUT_VARIABLE ROOST_ZIG_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
if(ROOST_ZIG_VERSION VERSION_LESS ROOST_ZIG_MIN_VERSION)
    message(FATAL_ERROR
        "zig ${ROOST_ZIG_VERSION} at ${ZIG_EXECUTABLE} is too old: "
        "libghostty-vt needs ${ROOST_ZIG_MIN_VERSION} or newer.")
endif()
message(STATUS "Found zig ${ROOST_ZIG_VERSION}: ${ZIG_EXECUTABLE}")

include(FetchContent)
FetchContent_Declare(ghostty
    GIT_REPOSITORY https://github.com/ghostty-org/ghostty.git
    GIT_TAG vt-${ROOST_GHOSTTY_VT_VERSION}
    GIT_SHALLOW TRUE
    SYSTEM
)
FetchContent_MakeAvailable(ghostty)

# libghostty-vt is linked statically into roost and roostd: its own install
# rules (a shared copy, headers, a CMake package) are not part of roost's
# install (cmake/Install.cmake). EXCLUDE_FROM_ALL drops them; the library is
# still built for what links it.
set_property(DIRECTORY ${ghostty_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
