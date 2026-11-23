cmake_minimum_required(VERSION 3.12.0 FATAL_ERROR)
project(ffmpeg)

# could put a build step here
# ExternalProject_Add(
#    PREFIX
#        FFmpeg
#
#)

# FFmpeg build to link: the in-tree one by default; scripts/build-win-cross.sh points this at a
# separate MinGW build so a Linux/Mac build in the same checkout is left alone
set(PP_FFMPEG_DIR ${CMAKE_CURRENT_LIST_DIR}/FFmpeg CACHE PATH "Built FFmpeg 4.4 source tree")

add_library(libavcodec STATIC IMPORTED GLOBAL)
add_library(libavformat STATIC IMPORTED GLOBAL)
add_library(libavutil STATIC IMPORTED GLOBAL)

set_target_properties(libavcodec
    PROPERTIES
        IMPORTED_LOCATION ${PP_FFMPEG_DIR}/libavcodec/libavcodec.a
)
set_target_properties(libavformat
    PROPERTIES
        IMPORTED_LOCATION ${PP_FFMPEG_DIR}/libavformat/libavformat.a
)
set_target_properties(libavutil
    PROPERTIES
        IMPORTED_LOCATION ${PP_FFMPEG_DIR}/libavutil/libavutil.a
)

find_library(CoreVideo CoreVideo)

target_link_libraries(libavutil
    INTERFACE $<$<PLATFORM_ID:Darwin>:${CoreVideo}>
)

target_include_directories(libavcodec INTERFACE
    ${PP_FFMPEG_DIR}
)
target_include_directories(libavformat INTERFACE
    ${PP_FFMPEG_DIR}
)
target_include_directories(libavutil INTERFACE
    ${PP_FFMPEG_DIR}
)

add_library(ffmpeg::libavcodec ALIAS libavcodec)
add_library(ffmpeg::libavformat ALIAS libavformat)
add_library(ffmpeg::libavutil ALIAS libavutil)

# add_include(ffmpeg ${CMAKE_CURRENT_LIST_DIR}/FFmpeg)
