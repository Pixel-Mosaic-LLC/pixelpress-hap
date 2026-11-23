# pphap - command-line encoder built from the same codec + QuickTime writer as the plugin
add_executable(pphap pphap.cpp)
target_compile_definitions(pphap PRIVATE PPHAP_VERSION="1.2.3" $<$<PLATFORM_ID:Windows>:NOMINMAX _CRT_SECURE_NO_WARNINGS>)
target_link_libraries(pphap PRIVATE CodecFoundationSession Codec CodecRegistration)
# 1.1: pphap selects the texture engine and reads the quality report (codec/texture_hq.hpp)
target_include_directories(pphap PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../codec)
# 1.1.7: Hap R decoding (Vidvox hap.c + bc7decomp, both already in the Codec library)
target_include_directories(pphap PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../external/bc7enc ${CMAKE_CURRENT_LIST_DIR}/../../external/hap)
if (APPLE)
    find_library(FoundationFramework Foundation)
    target_link_libraries(pphap PRIVATE ${FoundationFramework})
    # Mac installer puts it on the default PATH
    install(TARGETS pphap RUNTIME DESTINATION usr/local/bin COMPONENT plugin)
else ()
    install(TARGETS pphap RUNTIME DESTINATION . COMPONENT plugin)
    # 1.1.7: also in C:\Program Files\PixelPress (the "tools" component, see installer/CodecInstaller.cmake),
    # a fixed place other programs (Smart Preso) find through the registry
    install(TARGETS pphap RUNTIME DESTINATION . COMPONENT tools)
endif ()

if (WIN32)
    target_link_libraries(pphap PRIVATE shell32)   # 1.1.9: CommandLineToArgvW (UTF-8 arguments)
endif ()
if (MINGW)
    # cross-built pphap.exe: no MinGW runtime DLLs to ship
    target_link_options(pphap PRIVATE -static)
endif ()
