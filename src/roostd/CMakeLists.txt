# Everything of roostd but main(), so that tests drive the real thing.
add_library(roostd_lib STATIC
    manifest.cpp
    server.cpp
)

target_link_libraries(roostd_lib PUBLIC roost_core Qt6::Network)

qt_add_executable(roostd
    main.cpp
)

target_link_libraries(roostd PRIVATE roostd_lib)
