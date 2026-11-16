cmake_minimum_required(VERSION 3.25)
project(plugin_gate_fixture LANGUAGES NONE)

# Stand-ins: no Qt and no compiler are needed to run the gate.
add_library(Qt6::Core INTERFACE IMPORTED)
add_library(roost_core INTERFACE)

# plugins/shortcut -> src/core, made in the binary dir so the source tree
# stays clean; the plugin includes the link.
file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_BINARY_DIR}/shortcut" SYMBOLIC)

include(${ROOST_GATE_MODULE})
add_subdirectory(plugins)
