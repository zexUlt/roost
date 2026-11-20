# Everything of roostd but main(), so that tests drive the real thing.
add_library(roostd_lib STATIC
    adapter_bridge.cpp
    backend.cpp
    dump.cpp
    manifest.cpp
    plugins.cpp
    server.cpp
    session_channels.cpp
    store_service.cpp
    warm_pool.cpp
)

target_link_libraries(roostd_lib PUBLIC roost_core Qt6::Network)

qt_add_executable(roostd
    main.cpp
)

target_link_libraries(roostd PRIVATE roostd_lib)

set_target_properties(roostd PROPERTIES
    INSTALL_RPATH "${ROOST_INSTALL_RPATH}"
    INSTALL_RPATH_USE_LINK_PATH ${ROOST_INSTALL_RPATH_TO_QT}
)
install(TARGETS roostd RUNTIME DESTINATION ${ROOST_INSTALL_BINDIR} COMPONENT roost)
