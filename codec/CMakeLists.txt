cmake_minimum_required(VERSION 3.12.0 FATAL_ERROR)

project(Codec)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/BC7E.cmake)

# this is the one we will install
add_library(Codec
        codec.cpp
        codec.hpp
        gdeflate_hap.cpp
        gdeflate_hap.hpp
        gpu_bc7.cpp
        gpu_bc7.hpp
        gpu_bc7_spirv.h
        parallel.cpp
        parallel.hpp
        texture_converter.cpp
        texture_converter.hpp
        texture_hq.cpp
        texture_hq.hpp
        ${CMAKE_CURRENT_LIST_DIR}/../external/rgbcx/rgbcx.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../external/bc7enc/bc7decomp.cpp
        ${BC7E_OBJECTS}
        ${BC7E_INCLUDE_DIR}/bc7e_ispc.h
)
target_include_directories(Codec PRIVATE ${BC7E_INCLUDE_DIR} ${CMAKE_CURRENT_LIST_DIR}/../external/rgbcx ${CMAKE_CURRENT_LIST_DIR}/../external/bc7enc
                                         ${CMAKE_CURRENT_LIST_DIR}/../external/vulkan-headers/include)
add_dependencies(Codec bc7e_ispc)
if (MINGW)
    # Windows cross-build (scripts/build-win-cross.sh): ISPC's COFF objects reference __chkstk
    enable_language(ASM)
    target_sources(Codec PRIVATE mingw_chkstk.S)
endif ()
set_source_files_properties(texture_converter.cpp PROPERTIES OBJECT_DEPENDS ${BC7E_INCLUDE_DIR}/bc7e_ispc.h)
find_package(Threads REQUIRED)

target_link_libraries(Codec
    CodecRegistration
    Hap
    GDeflate
    snappy
    squish
    ycocg
    Threads::Threads
    ${CMAKE_DL_LIBS}   # gpu_bc7.cpp loads the Vulkan driver at run time (dlopen; LoadLibrary on Windows)
)

target_include_directories(Codec
    PRIVATE
    INTERFACE
        CodecRegistration
)

# hack because squish CMakeLists.txt predates v3.4
# TODO: fix the squish CMakeLists.txt
list(APPEND INCLUDE_DIRS ${squish_SOURCE_DIR})
include_directories(${INCLUDE_DIRS})