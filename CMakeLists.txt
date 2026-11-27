cmake_minimum_required(VERSION 3.25)

# The patch of the version is not in this file: the night and a release dispatch
# of package.yml take the next one from the tags (tools/package_plan.sh) and pass
# it in; a build by hand is MAJOR.MINOR.0. The minor is raised in a pull request.
set(ROOST_VERSION_PATCH 0 CACHE STRING "Patch of the project version")
if(NOT ROOST_VERSION_PATCH MATCHES "^(0|[1-9][0-9]*)$")
    message(FATAL_ERROR
        "ROOST_VERSION_PATCH must be a non-negative integer without leading "
        "zeros, found '${ROOST_VERSION_PATCH}'.")
endif()

project(roost
    VERSION 0.6.${ROOST_VERSION_PATCH}
    DESCRIPTION "A desktop app over background coding-agent sessions"
    LANGUAGES C CXX
)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Use a preset: cmake --preset dev")
endif()

# Clang only: one compiler, one set of diagnostics, one set of sanitizers.
# On macOS this is Apple clang from Xcode, which reports itself as AppleClang.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "roost builds with Clang; found ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION}. Configure through a preset "
        "(cmake --preset dev), which pins the compiler.")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
    message(FATAL_ERROR "Clang 17 or newer is required, found ${CMAKE_CXX_COMPILER_VERSION}.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Binaries in one place: build/<preset>/bin. Also keeps the executable `roost`
# away from the QML module directory `Roost/` that qt_add_qml_module writes
# next to it, which is the same path on a case-insensitive macOS file system.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

option(ROOST_BUILD_TESTS "Build the test suite" ON)
option(ROOST_WERROR "Treat warnings as errors" ON)

# Only modules available under LGPLv3, linked dynamically (design.md §10).
# 6.8 is the floor for QQuickWindow::createTextNode() and the CMake API used
# here; CI builds against one pinned version.
# QmlPrivate: used only by src/qml_api/calling_context.cpp. Qt warns that a
# private module ties the build to one Qt version; that is accepted there, CI
# pins the version. The warning is silenced for this call only.
# QuickPrivate: used only by tests/qml_api/terminal_grid_tree_test.cpp, which
# listens to the notifications a scene graph sends to its renderer. Optional:
# it needs GuiPrivate (Vulkan headers, XKB), missing in the Linux CI images, and
# the test is built only where the module is found.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
# DBus: the desktop notifications of Linux (org.freedesktop.Notifications,
# src/roost/desktop_notifier_linux.cpp); required there, optional elsewhere,
# where the same code is built only to be tested.
# A component is never both required and optional: find_package fails on that.
set(_roost_qt_required Core Gui Network Qml Quick QuickControls2 QmlPrivate)
set(_roost_qt_optional QuickPrivate)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND _roost_qt_required DBus)
else()
    list(APPEND _roost_qt_optional DBus)
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS ${_roost_qt_required}
    OPTIONAL_COMPONENTS ${_roost_qt_optional})
unset(QT_NO_PRIVATE_MODULE_WARNING)
qt_standard_project_setup(REQUIRES 6.8)

include(cmake/CompileOptions.cmake)
include(cmake/ghostty.cmake)
include(cmake/Install.cmake)
include(cmake/PluginGate.cmake)

add_subdirectory(third_party/monocypher)

add_subdirectory(src)
add_subdirectory(plugins)

if(ROOST_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
