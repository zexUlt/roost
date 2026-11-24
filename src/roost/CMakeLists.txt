# The window's controllers that are not QML types; shared by the executable and
# the tests.
add_library(roost_app STATIC
    bug_report.cpp
    bug_report.hpp
    controls_style.cpp
    controls_style.hpp
    daemon_launcher.cpp
    daemon_launcher.hpp
    key_matcher.cpp
    key_matcher.hpp
    layout_commands.cpp
    layout_commands.hpp
    layout_model.cpp
    layout_model.hpp
    login_environment.cpp
    login_environment.hpp
    plugin_loader.cpp
    plugin_loader.hpp
    plugin_watcher.cpp
    plugin_watcher.hpp
    settings_pages.cpp
    settings_pages.hpp
    shell.cpp
    shell.hpp
    single_session.cpp
    single_session.hpp
    window_icon.cpp
    window_icon.hpp
    window_log.cpp
    window_log.hpp
)
target_link_libraries(roost_app PUBLIC roost_qml_api roost_terminal Qt6::Quick Qt6::QuickControls2)

# The icon of the window: the PNGs of the Linux package, so that there is one
# set of files to replace (tools/make_icons.sh). PNG, not SVG: no image plugin.
set(_roost_icons ${PROJECT_SOURCE_DIR}/packaging/linux/icons/hicolor)
set(_roost_icon_files)
foreach(size 16 32 48 64 128 256 512)
    set(file ${_roost_icons}/${size}x${size}/apps/roost.png)
    set_source_files_properties(${file} PROPERTIES QT_RESOURCE_ALIAS ${size}.png)
    list(APPEND _roost_icon_files ${file})
endforeach()
qt_add_resources(roost_app window_icons
    PREFIX /roost/icons
    FILES ${_roost_icon_files})

# The QML of the window, as a static library so that the tests can load
# Main.qml too (an executable cannot be linked). Link roost_shell and
# roost_shellplugin and use Q_IMPORT_QML_PLUGIN(Roost_ShellPlugin).
#
# NO_CACHEGEN: qmlcachegen compiles QML bindings to C++ ahead of time, and v1
# does not use AOT until its licence is settled (design.md §10, §14). QML is
# compiled at run time instead, as plugin QML is anyway.
qt_add_library(roost_shell STATIC)
qt_add_qml_module(roost_shell
    URI Roost.Shell
    VERSION 1.0
    QML_FILES BugReportDialog.qml CommandPalette.qml FormButton.qml FormComboBox.qml
        FormField.qml LayoutNode.qml LayoutView.qml LinkEditor.qml Main.qml MenuRow.qml
        MenuSeparatorRow.qml PanelCard.qml PanelMenu.qml PanelSlot.qml RoostMenu.qml
        SessionMain.qml SettingsWindow.qml SplitNode.qml TabNode.qml ToolbarButton.qml
    SOURCES bug_report_controller.cpp bug_report_controller.hpp
            palette_model.cpp palette_model.hpp
    NO_CACHEGEN
)
target_link_libraries(roost_shell PUBLIC roost_app Qt6::Quick)

qt_add_executable(roost
    main.cpp
)

target_link_libraries(roost PRIVATE roost_core roost_app
    roost_shell roost_shellplugin roost_qml_apiplugin
    Qt6::Gui Qt6::Qml Qt6::Quick)

# Not a bundle in the build tree: build/<preset>/bin/roost stays a plain
# executable. cmake/Install.cmake assembles roost.app at install time.
set_target_properties(roost PROPERTIES
    MACOSX_BUNDLE OFF
    INSTALL_RPATH "${ROOST_INSTALL_RPATH}"
    INSTALL_RPATH_USE_LINK_PATH ${ROOST_INSTALL_RPATH_TO_QT}
)
install(TARGETS roost RUNTIME DESTINATION ${ROOST_INSTALL_BINDIR} COMPONENT roost)
