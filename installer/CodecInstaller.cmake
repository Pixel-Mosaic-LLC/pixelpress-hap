cmake_minimum_required(VERSION 3.12.0 FATAL_ERROR)

set(CPACK_PACKAGE_NAME "PixelPressHAP")
set(CPACK_PACKAGE_VENDOR "Pixel Mosaic")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "PixelPress HAP - Hap / Hap Alpha / Hap Q / Hap R export plugin for Adobe After Effects, Premiere Pro and Media Encoder")
set(CPACK_PACKAGE_VERSION "1.2.3")
set(CPACK_PACKAGE_VERSION_MAJOR "1")
set(CPACK_PACKAGE_VERSION_MINOR "1")
set(CPACK_PACKAGE_VERSION_PATCH "9")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "PixelPressHAP")
# Installer file names always carry the version, e.g. PixelPressHAP-1.1.8-macOS-AppleSilicon.pkg /
# PixelPressHAP-1.1.8-Windows-x64.exe, so it's obvious which one is being installed.
if(APPLE)
  set(CPACK_PACKAGE_FILE_NAME "PixelPressHAP-${CPACK_PACKAGE_VERSION}-macOS-AppleSilicon")
elseif(WIN32)
  set(CPACK_PACKAGE_FILE_NAME "PixelPressHAP-${CPACK_PACKAGE_VERSION}-Windows-x64")
endif()
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_LIST_DIR}/../license.txt)
set(CPACK_RESOURCE_FILE_README ${CMAKE_CURRENT_LIST_DIR}/ReadMe.txt)

# 1.1.4: also install the licence / third-party notices as a file, so they stay with the
# installed plugin (BSD/MIT/Apache notice requirements, LGPL source notice)
if (APPLE)
    install(FILES ${CMAKE_CURRENT_LIST_DIR}/../license.txt
            DESTINATION "Library/Application Support/PixelPressHAP"
            RENAME "PixelPressHAP-Licenses.txt" COMPONENT plugin)
else ()
    install(FILES ${CMAKE_CURRENT_LIST_DIR}/../license.txt
            DESTINATION . RENAME "PixelPressHAP-Licenses.txt" COMPONENT plugin)
endif ()

if (APPLE)
    set(CPACK_PREFLIGHT_PLUGIN_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/preinstall")
    # Sign the .pkg only when a Developer ID Installer identity is supplied:
    #   PPHAP_INSTALLER_IDENTITY="Developer ID Installer: Your Name (TEAMID)"
    if (DEFINED ENV{PPHAP_INSTALLER_IDENTITY})
        set(CPACK_PRODUCTBUILD_IDENTITY_NAME "$ENV{PPHAP_INSTALLER_IDENTITY}")
    endif ()
endif (APPLE)

# 1.1.7 Windows: a fixed, version-independent place for other programs (Smart Preso) - the plugin
# itself has to stay in Adobe's MediaCore folder, whose path is Adobe's to change. The installer puts
#   pphap.exe, ffmpeg.exe, ffprobe.exe (LGPL, scripts/build-ffmpeg-win-exe.sh) and the licences
# in C:\Program Files\PixelPress ("tools" component, always installed) and writes, in the 64-bit view,
#   HKLM\SOFTWARE\Pixel Mosaic\PixelPress HAP   PphapPath, InstallDir, Version, PluginDir
# Uninstalling removes both. Reinstalling / upgrading replaces them (uninstall-before-install).
if (WIN32)
    set(PP_FFMPEG_WIN_DIR "${CMAKE_CURRENT_LIST_DIR}/../dist/ffmpeg-win" CACHE PATH
        "folder with the LGPL ffmpeg.exe / ffprobe.exe the Windows installer bundles")
    install(FILES ${CMAKE_CURRENT_LIST_DIR}/../license.txt
            DESTINATION . RENAME "PixelPressHAP-Licenses.txt" COMPONENT tools)
    if (EXISTS "${PP_FFMPEG_WIN_DIR}/ffmpeg.exe" AND EXISTS "${PP_FFMPEG_WIN_DIR}/ffprobe.exe")
        install(FILES "${PP_FFMPEG_WIN_DIR}/ffmpeg.exe" "${PP_FFMPEG_WIN_DIR}/ffprobe.exe"
                DESTINATION . COMPONENT tools)
        install(FILES "${PP_FFMPEG_WIN_DIR}/build-info.txt"
                DESTINATION . RENAME "ffmpeg-build-info.txt" COMPONENT tools)
    else ()
        message(WARNING "No ffmpeg.exe / ffprobe.exe in ${PP_FFMPEG_WIN_DIR}: the installer will not include them "
                        "(run scripts/build-ffmpeg-win-exe.sh, or set PP_FFMPEG_WIN_DIR)")
    endif ()
    # 1.1.8: pphap.exe needs the Visual C++ runtime (vcruntime140 / msvcp140). Windows doesn't always
    # have it (it usually arrives with Adobe apps), so its DLLs go next to pphap.exe (Microsoft's
    # "app-local" deployment) - pphap then runs on a clean machine with nothing else installed.
    # The Universal CRT is part of Windows 10/11, so it isn't copied.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT tools)
    set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
    include(InstallRequiredSystemLibraries)
    if (NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        message(WARNING "Visual C++ runtime DLLs not found: pphap.exe will need the VC++ redistributable")
    endif ()
    set(Foundation_CUSTOM_INSTALLER_COMPONENTS tools)
    set(CPACK_NSIS_tools_INSTALL_DIRECTORY "$PROGRAMFILES64\\\\PixelPress")
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
  ; PixelPress 1.1.7: where other programs find pphap.exe (64-bit registry view)
  SetRegView 64
  WriteRegStr HKLM 'Software\\\\Pixel Mosaic\\\\PixelPress HAP' 'PphapPath' '$PROGRAMFILES64\\\\PixelPress\\\\pphap.exe'
  WriteRegStr HKLM 'Software\\\\Pixel Mosaic\\\\PixelPress HAP' 'InstallDir' '$PROGRAMFILES64\\\\PixelPress'
  WriteRegStr HKLM 'Software\\\\Pixel Mosaic\\\\PixelPress HAP' 'Version' '${CPACK_PACKAGE_VERSION}'
  WriteRegStr HKLM 'Software\\\\Pixel Mosaic\\\\PixelPress HAP' 'PluginDir' '$PROGRAMFILES64\\\\Adobe\\\\Common\\\\Plug-ins\\\\7.0\\\\MediaCore\\\\${Foundation_CODEC_NAME}'
  SetRegView 32
")
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
  ; PixelPress 1.1.7: the fixed tools folder and its registry entry
  Delete '$PROGRAMFILES64\\\\PixelPress\\\\pphap.exe'
  Delete '$PROGRAMFILES64\\\\PixelPress\\\\ffmpeg.exe'
  Delete '$PROGRAMFILES64\\\\PixelPress\\\\ffprobe.exe'
  Delete '$PROGRAMFILES64\\\\PixelPress\\\\ffmpeg-build-info.txt'
  Delete '$PROGRAMFILES64\\\\PixelPress\\\\PixelPressHAP-Licenses.txt'
  Delete '$PROGRAMFILES64\\\\PixelPress\\\\vcruntime140*.dll'
  Delete '$PROGRAMFILES64\\\\PixelPress\\\\msvcp140*.dll'
  Delete '$PROGRAMFILES64\\\\PixelPress\\\\concrt140.dll'
  Delete '$PROGRAMFILES64\\\\PixelPress\\\\vccorlib140.dll'
  RMDir '$PROGRAMFILES64\\\\PixelPress'
  SetRegView 64
  DeleteRegKey HKLM 'Software\\\\Pixel Mosaic\\\\PixelPress HAP'
  DeleteRegKey /ifempty HKLM 'Software\\\\Pixel Mosaic'
  SetRegView 32
")
endif ()

# NSIS (Windows installer) settings
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_PACKAGE_NAME "PixelPress HAP Plugin for Adobe CC")
set(CPACK_NSIS_DISPLAY_NAME "PixelPress HAP Plugin for Adobe CC")
# NOTE: deliberately no CPACK_NSIS_EXTRA_INSTALL_COMMANDS - the upstream installer deleted
# the disguise HapEncoderPlugin.prm, and this plugin is meant to coexist with it.
