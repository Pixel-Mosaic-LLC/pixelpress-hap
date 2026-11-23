cmake_minimum_required(VERSION 3.12.0 FATAL_ERROR)
project(bc7enc)

# Rich Geldreich's portable BC7 encoder (MIT / public domain) - used for Hap R
add_library(bc7enc STATIC
    bc7enc.cpp
    bc7enc.h
)
target_include_directories(bc7enc PUBLIC ${CMAKE_CURRENT_LIST_DIR})
set_target_properties(bc7enc PROPERTIES POSITION_INDEPENDENT_CODE ON)

# bc7decomp.cpp/.h are kept only for the encoder self-test (tools/), not linked into the plugin.
