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

# Zig builds for the CPU of the build machine unless told otherwise, and a
# package runs on other machines: libghostty-vt built on a runner with AVX-512
# dies of SIGILL on one without. Pin the CPU, unless the flags already name a
# CPU or a target. The levels are the floor of what roost runs on:
#   x86_64         x86_64_v3 (AVX2: Intel Haswell, 2013, or AMD Zen and newer)
#   Linux aarch64  baseline
#   Apple arm64    apple_m1 (ghostty's own build replaces the target on a Mac
#                  with its generic one, so this changes nothing there today)
# The SIMD code of libghostty-vt (highway, simdutf) picks AVX2 or AVX-512 at
# run time, whatever the CPU of the build.
if(NOT GHOSTTY_ZIG_BUILD_FLAGS MATCHES "-D(cpu|target)=")
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" ROOST_ZIG_ARCH)
    if(ROOST_ZIG_ARCH MATCHES "^(x86_64|amd64)$")
        set(ROOST_ZIG_CPU x86_64_v3)
    elseif(APPLE AND ROOST_ZIG_ARCH MATCHES "^(arm64|aarch64)$")
        set(ROOST_ZIG_CPU apple_m1)
    elseif(ROOST_ZIG_ARCH MATCHES "^(arm64|aarch64)$")
        set(ROOST_ZIG_CPU baseline)
    else()
        message(FATAL_ERROR "no Zig CPU is pinned for ${CMAKE_SYSTEM_PROCESSOR}: "
            "add one to cmake/ghostty.cmake or pass -Dcpu= in GHOSTTY_ZIG_BUILD_FLAGS")
    endif()
    # In the cache: ghostty's own `set(... CACHE)` of this variable would
    # drop a normal variable.
    list(APPEND GHOSTTY_ZIG_BUILD_FLAGS "-Dcpu=${ROOST_ZIG_CPU}")
    set(GHOSTTY_ZIG_BUILD_FLAGS "${GHOSTTY_ZIG_BUILD_FLAGS}" CACHE STRING
        "Additional flags to pass to zig build" FORCE)
    message(STATUS "libghostty-vt: zig -Dcpu=${ROOST_ZIG_CPU}")
endif()

FetchContent_MakeAvailable(ghostty)

# libghostty-vt is linked statically into roost and roostd: its own install
# rules (a shared copy, headers, a CMake package) are not part of roost's
# install (cmake/Install.cmake). EXCLUDE_FROM_ALL drops them; the library is
# still built for what links it.
set_property(DIRECTORY ${ghostty_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
