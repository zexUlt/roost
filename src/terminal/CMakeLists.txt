# The window side of a terminal: what owns one and who may touch it. No GUI
# yet; the Qt Quick item that draws it comes later (design.md §13, M3).
add_library(roost_terminal STATIC
    terminal_session.cpp
)

target_link_libraries(roost_terminal PUBLIC roost_core)
