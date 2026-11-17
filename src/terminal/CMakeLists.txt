# The window side of a terminal: what owns one, who may touch it, and how it
# is drawn into the scene graph. The Qt Quick item that hosts the renderer is
# in src/qml_api.
add_library(roost_terminal STATIC
    io_thread.cpp
    qt_key_mapping.cpp
    renderer.cpp
    session_pane.cpp
    terminal_session.cpp
)

target_link_libraries(roost_terminal PUBLIC roost_core Qt6::Quick)
