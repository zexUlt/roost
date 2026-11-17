# The window's controllers that are not QML types; shared by the executable and
# the tests.
add_library(roost_app STATIC
    plugin_loader.cpp
    plugin_loader.hpp
    single_session.cpp
    single_session.hpp
)
target_link_libraries(roost_app PUBLIC roost_qml_api roost_terminal Qt6::Quick)

qt_add_executable(roost
    main.cpp
)

# NO_CACHEGEN: qmlcachegen compiles QML bindings to C++ ahead of time, and v1
# does not use AOT until its licence is settled (design.md §10, §14). QML is
# compiled at run time instead, as plugin QML is anyway.
qt_add_qml_module(roost
    URI Roost.Shell
    QML_FILES Main.qml SessionMain.qml
    NO_CACHEGEN
)

target_link_libraries(roost PRIVATE roost_core roost_app roost_qml_apiplugin
    Qt6::Gui Qt6::Qml Qt6::Quick)

set_target_properties(roost PROPERTIES
    MACOSX_BUNDLE OFF
)
