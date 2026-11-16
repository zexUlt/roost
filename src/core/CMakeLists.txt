# What roost and roostd share. No GUI: QtCore, and QtNetwork for the socket.
add_library(roost_core STATIC
    bus.cpp
    frame.cpp
    ghostty_result.cpp
    protocol.cpp
    pty.cpp
    rpc.cpp
    rpc_client.cpp
    store.cpp
    version.cpp
    vt_render_state.cpp
    vt_terminal.cpp
)

target_include_directories(roost_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(roost_core PRIVATE ROOST_VERSION="${PROJECT_VERSION}")
target_link_libraries(roost_core
    PUBLIC roost::compile_options Qt6::Core Qt6::Network
    PRIVATE ghostty-vt-static
)

# openpty() lives in libutil on Linux and in libc on macOS.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(roost_core PRIVATE util)
endif()
