# GDeflate (PixelPress HAP 1.2.2): Microsoft DirectStorage's GDeflate reference codec (Apache-2.0,
# GDeflate/) on NVIDIA's libdeflate fork (MIT, libdeflate/). Only what the CPU codec needs.
# Source: https://github.com/microsoft/DirectStorage (GDeflate/), commits in VERSIONS.txt
cmake_minimum_required(VERSION 3.12.0 FATAL_ERROR)
project(GDeflate C CXX)
set(LD ${CMAKE_CURRENT_LIST_DIR}/libdeflate)
add_library(GDeflate STATIC
    GDeflate/GDeflateCompress.cpp
    GDeflate/GDeflateDecompress.cpp
    ${LD}/lib/deflate_compress.c
    ${LD}/lib/deflate_decompress.c
    ${LD}/lib/gdeflate_compress.c
    ${LD}/lib/gdeflate_decompress.c
    ${LD}/lib/utils.c
    ${LD}/lib/x86/cpu_features.c
    ${LD}/lib/arm/cpu_features.c          # Apple Silicon / ARM64 (empty on x64)
    ${LD}/lib/adler32.c
    ${LD}/lib/crc32.c
)
target_include_directories(GDeflate PUBLIC ${CMAKE_CURRENT_LIST_DIR}/GDeflate PRIVATE ${LD})
target_compile_features(GDeflate PRIVATE cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(GDeflate PRIVATE Threads::Threads)
if (MSVC)
    target_compile_options(GDeflate PRIVATE /W0)
else ()
    target_compile_options(GDeflate PRIVATE -w)
endif ()
