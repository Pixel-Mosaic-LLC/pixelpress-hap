# Stand-alone self-test for the Hap R encode path (no Adobe SDK / FFmpeg needed).
#   cmake -S tools -B build-selftest -DISPC_EXECUTABLE=/path/to/ispc && cmake --build build-selftest
cmake_minimum_required(VERSION 3.15)
project(HapRSelfTest CXX C)
set(CMAKE_CXX_STANDARD 17)
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(SNAPPY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SQUISH_WITH_SSE2 OFF CACHE BOOL "" FORCE)   # exercise the portable (Apple Silicon) path
add_subdirectory(${ROOT}/external/snappy snappy)
add_subdirectory(${ROOT}/external/squish/squish-source squish)
add_subdirectory(${ROOT}/external/hap hap)
add_subdirectory(${ROOT}/external/ycocg ycocg)
include(${ROOT}/cmake/BC7E.cmake)
find_package(Threads REQUIRED)
add_library(selftest_codec STATIC
    ${ROOT}/codec/texture_converter.cpp
    ${ROOT}/codec/parallel.cpp
    ${ROOT}/codec/texture_hq.cpp
    ${ROOT}/codec/gpu_bc7.cpp          # 1.2.0: texture_converter can use the GPU encoder (off by default)
    ${ROOT}/external/rgbcx/rgbcx.cpp
    ${ROOT}/external/bc7enc/bc7decomp.cpp
    ${BC7E_OBJECTS})
target_include_directories(selftest_codec PUBLIC
    ${ROOT}/codec ${ROOT}/external/foundation/source/codec_registration
    ${ROOT}/external/squish/squish-source ${ROOT}/external/ycocg ${BC7E_INCLUDE_DIR}
    ${ROOT}/external/rgbcx ${ROOT}/external/bc7enc ${ROOT}/external/vulkan-headers/include)
target_link_libraries(selftest_codec PUBLIC Hap snappy squish ycocg Threads::Threads ${CMAKE_DL_LIBS})
add_dependencies(selftest_codec bc7e_ispc)
add_executable(hapr_selftest hapr_selftest.cpp)
target_include_directories(hapr_selftest PRIVATE ${ROOT}/external/bc7enc)
target_link_libraries(hapr_selftest selftest_codec)
