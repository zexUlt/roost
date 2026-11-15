# What roost and roostd share. No GUI: roostd links QtCore only.
add_library(roost_core STATIC
    bus.cpp
    frame.cpp
    rpc.cpp
    version.cpp
)

target_include_directories(roost_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(roost_core PRIVATE ROOST_VERSION="${PROJECT_VERSION}")
target_link_libraries(roost_core
    PUBLIC roost::compile_options Qt6::Core
    PRIVATE ghostty-vt-static
)
