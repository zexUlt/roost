cmake_minimum_required(VERSION 3.25)

project(roost
    VERSION 0.2.0
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
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Network Qml Quick QmlPrivate)
unset(QT_NO_PRIVATE_MODULE_WARNING)
qt_standard_project_setup(REQUIRES 6.8)

include(cmake/CompileOptions.cmake)
include(cmake/ghostty.cmake)
include(cmake/Install.cmake)
include(cmake/PluginGate.cmake)

add_subdirectory(src)
add_subdirectory(plugins)

if(ROOST_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
