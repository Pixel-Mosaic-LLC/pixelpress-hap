cmake_minimum_required(VERSION 3.15.0 FATAL_ERROR)


# project-wide build settings that MUST BE SET PRIOR TO FIRST 'project'
# ----------------------------

# macOS: Apple Silicon (M-series) native build. macOS 11 is the first release on
# Apple Silicon and has std::filesystem, so no Boost is required.
# To build an Intel Mac version instead: -DCMAKE_OSX_ARCHITECTURES=x86_64
if(CMAKE_HOST_APPLE)   # APPLE itself isn't defined until project()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment version")
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS architecture" FORCE)
    endif()
endif()


# project
# ----------------------------
project(CodecPlugin)

# foundation variables
# ----------------------------

# PixelPress HAP: own identifiers so it installs side by side with the disguise HAP plugin
set(Foundation_IDENTIFIER_PREFIX com.pixelmosaic.PixelPressHAP)
set(Foundation_OBJC_PREFIX PXPPHAP)   # unique Objective-C class prefix (coexists with other foundation plugins)
set(Foundation_CODEC_NAME PPHAP)
# (the disguise Media Encoder .epr presets reference the original plugin's IDs, so none are shipped)
set(Foundation_FILE_IMPORT_FOUR_CC 0x50504800L)    # hex for PPH\0, unique importer id
set(Foundation_CODEC_NAME_WITH_HEX_SIZE_PREFIX "\"\\x07PPHAP\\0\\0\"")    # PiPL name: size byte 0x7 + 7 bytes ("PPHAP" padded)

# project-wide build settings
# ----------------------------

# all platforms: group targets in IDEs
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(MINGW)
    # Windows cross-build (scripts/build-win-cross.sh): Windows.h/ShlObj.h spelled as on Windows,
    # StackWalker stand-in
    include_directories(BEFORE SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/cmake/mingw)
endif()
if(MSVC)
    # add_compile_options(/arch:AVX2)
    # may want to use this for specific source files
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    # Apple Silicon: no x86 SIMD flags (squish/YCoCg/bc7enc fall back to portable C++)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    add_compile_options(-msse4.1)
endif()

set (CMAKE_CXX_STANDARD 17)

option(Foundation_PACKAGE_TESTS "Build the foundation tests (needs external/foundation/external/googletest)" OFF)

# foundation interface to be populated
# ----------------------------

# - externals needed by codec registration interface
add_subdirectory(external/foundation/external)
# - defines codec registration interface
add_subdirectory(external/foundation/source/codec_registration)


# codec-specific external libs
# ----------------------------

set(SNAPPY_BUILD_TESTS FALSE)
add_subdirectory(external/snappy)

add_subdirectory(external/squish/squish-source)
add_subdirectory(external/hap)
add_subdirectory(external/gdeflate)   # 1.2.2: optional GDeflate chunk compression (non-standard Hap)
add_subdirectory(external/ycocg)

# codec implementation in its own library
# ----------------------------
add_subdirectory(codec)

# foundation plugins
# ----------------------------
add_subdirectory(external/foundation)

# command-line encoder (pphap)
# ----------------------------
add_subdirectory(tools/cli)

# codec installer information for foundation
# ----------------------------
include(installer/CodecInstaller.cmake)

# foundation installer framework
# *** this must be last according to CPACK usage directions
add_subdirectory(external/foundation/installer)

# codec-specific components
# must be added *after* CPACK usage in external/foundation/installer above
if (WIN32)
    cpack_add_component(tools
        DISPLAY_NAME "Command-line tools"
        DESCRIPTION "pphap.exe and ffmpeg in the PixelPress folder in Program Files, for programs such as Smart Preso"
        REQUIRED)
endif ()
