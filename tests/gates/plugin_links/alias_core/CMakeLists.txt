cmake_minimum_required(VERSION 3.25)
project(plugin_gate_fixture LANGUAGES NONE)

# Stand-ins: no Qt and no compiler are needed to run the gate.
add_library(Qt6::Core INTERFACE IMPORTED)
add_library(roost_core INTERFACE)
add_library(roost::core ALIAS roost_core)

include(${ROOST_GATE_MODULE})
add_subdirectory(plugins)
