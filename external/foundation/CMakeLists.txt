cmake_minimum_required(VERSION 3.15.0 FATAL_ERROR)

# ide layout
set(CMAKE_FOLDER foundation)

# modules

# project

project(CodecFoundation)

set (CMAKE_CXX_STANDARD 17)

if(MSVC)
    # add_compile_options(/arch:AVX2)
    # may want to use this for specific source files
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    # Apple Silicon: no x86 SIMD flags
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    add_compile_options(-msse4.1)
endif()

# library dependencies
# !!! already included - add_subdirectory(external)

option(Foundation_PACKAGE_TESTS "Build the tests" OFF)

# documentation
# add_subdirectory(doc)

# plugins
add_subdirectory(source)


