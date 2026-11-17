# The API of the window (design.md §5, "Один API"): the services and the
# by-name method dispatcher. The QML singletons Roost.* and the host of the C
# SDK are adapters over it, without logic of their own.
add_library(roost_api STATIC
    api.cpp
    bus_service.cpp
    command_service.cpp
    dispatcher.cpp
    plugin_service.cpp
    session_service.cpp
    store_service.cpp
    thread_check.cpp
)

target_link_libraries(roost_api PUBLIC roost_terminal Qt6::Core)
