add_library(roost_plugin_backend STATIC backend_process.cpp json_rpc.cpp)
target_include_directories(roost_plugin_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(roost_plugin_backend PUBLIC Qt6::Core)
