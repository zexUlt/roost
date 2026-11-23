# What roost and roostd share. No GUI: QtCore, and QtNetwork for the socket.
add_library(roost_core STATIC
    appimage_environment.cpp
    appimage_install.cpp
    bus.cpp
    frame.cpp
    ghostty_result.cpp
    key_chord.cpp
    log_file.cpp
    protocol.cpp
    pty.cpp
    rpc.cpp
    rpc_client.cpp
    store.cpp
    terminal_theme.cpp
    theme_source.cpp
    update_verify.cpp
    version.cpp
    vt_key_encoder.cpp
    vt_mouse_encoder.cpp
    vt_render_state.cpp
    vt_terminal.cpp
)

target_include_directories(roost_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(roost_core PRIVATE ROOST_VERSION="${PROJECT_VERSION}")
target_link_libraries(roost_core
    PUBLIC roost::compile_options Qt6::Core Qt6::Network
    PRIVATE ghostty-vt-static monocypher
)

# openpty() lives in libutil on Linux and in libc on macOS.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(roost_core PRIVATE util)
endif()

# The built-in themes, for the window and for roostd (src/core/theme_source.hpp).
qt_add_resources(roost_core roost_themes
    PREFIX /roost
    FILES themes/default.json
)
