# Monocypher (CC0-1.0 or BSD-2-Clause), vendored: the Ed25519 and BLAKE2b that
# check the signature of a downloaded release (src/core/update_verify.cpp).
# Plain C, built as it is shipped: the warning set of roost::compile_options
# is for our own code, as for the imported libghostty-vt.
add_library(monocypher STATIC
    monocypher.c
    optional/monocypher-ed25519.c
)
target_include_directories(monocypher SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(monocypher PROPERTIES
    C_STANDARD 99
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
