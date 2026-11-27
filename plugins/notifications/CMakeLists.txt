# The notifications plugin (M8): a backend that runs the user's commands when a
# session needs a human, is done or failed. build/<preset>/roost-plugins/
# notifications holds plugin.json and backend/notifications-backend.
set(ROOST_NOTIFICATIONS_PLUGIN_DIR ${ROOST_PLUGINS_OUTPUT_DIR}/notifications)

# Everything of the backend but main(), so that tests drive the real thing.
add_library(notifications_backend_lib STATIC
    backend/command_runner.cpp
    backend/diagnostics.cpp
    backend/notifier.cpp
    backend/settings.cpp
)
target_include_directories(notifications_backend_lib PUBLIC ${PROJECT_SOURCE_DIR}/plugins)
target_link_libraries(notifications_backend_lib PUBLIC roost_plugin_backend)

qt_add_executable(notifications-backend backend/main.cpp)
target_link_libraries(notifications-backend PRIVATE notifications_backend_lib Qt6::Core)
set_target_properties(notifications-backend PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ROOST_NOTIFICATIONS_PLUGIN_DIR}/backend)

configure_file(plugin.json ${ROOST_NOTIFICATIONS_PLUGIN_DIR}/plugin.json COPYONLY)
# The settings page of the plugin (contributes.settings): plain QML, read by
# the window from the directory of the plugin.
configure_file(qml/NotificationsSettings.qml
    ${ROOST_NOTIFICATIONS_PLUGIN_DIR}/qml/NotificationsSettings.qml COPYONLY)

install(FILES plugin.json DESTINATION ${ROOST_INSTALL_PLUGINS_DIR}/notifications
    COMPONENT roost)
install(FILES qml/NotificationsSettings.qml
    DESTINATION ${ROOST_INSTALL_PLUGINS_DIR}/notifications/qml COMPONENT roost)
roost_install_plugin_binary(notifications notifications-backend backend)
