# Compiles external/bc7e/bc7e.ispc with Intel ISPC (https://ispc.github.io) and sets
#   BC7E_OBJECTS      object files to add to a target's sources
#   BC7E_INCLUDE_DIR  directory containing the generated bc7e_ispc.h
# Include this from the CMakeLists.txt of the target that uses the objects (custom-command
# outputs must be consumed in the same directory for the Visual Studio generator).
#
# ISPC is found on PATH, via -DISPC_EXECUTABLE=..., or $ISPC_ROOT/bin.
# Targets: Apple Silicon -> ARM NEON; x64 -> SSE4 + AVX2 + AVX-512 with runtime dispatch (the best
# one the CPU supports is picked when the encoder starts).

find_program(ISPC_EXECUTABLE ispc HINTS ENV ISPC_ROOT PATH_SUFFIXES bin)
if (NOT ISPC_EXECUTABLE)
    message(FATAL_ERROR "Intel ISPC compiler not found. The build scripts download it automatically; "
                        "otherwise get it from https://github.com/ispc/ispc/releases and pass -DISPC_EXECUTABLE=/path/to/ispc")
endif ()

set(BC7E_SOURCE ${CMAKE_CURRENT_LIST_DIR}/../external/bc7e/bc7e.ispc)
set(BC7E_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/bc7e)

if (WIN32)
    set(_bc7e_ext .obj)
    set(_bc7e_os windows)
elseif (APPLE)
    set(_bc7e_ext .o)
    set(_bc7e_os macos)
else ()
    set(_bc7e_ext .o)
    set(_bc7e_os linux)
endif ()

if ((APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64") OR
    (NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$"))
    # neon-i32x4 or neon-i32x8 (same output; build-mac.sh times both on the build machine
    # and passes the faster one)
    set(BC7E_NEON_TARGET neon-i32x4 CACHE STRING "ISPC target for bc7e on ARM64")
    set(_bc7e_args --arch=aarch64 --target=${BC7E_NEON_TARGET})
    set(BC7E_OBJECTS ${BC7E_INCLUDE_DIR}/bc7e${_bc7e_ext})
else ()
    # 1.2.0: + avx512skx-x16 (Intel Skylake-X and later, AMD Zen 4 / Threadripper 7000 and later).
    # Same quality as AVX2 (identical PSNR to 0.001 dB on the test clips; blocks can differ in the
    # last bit because AVX-512 rounds a few reciprocals differently), 25-33% faster on Intel.
    set(_bc7e_args --arch=x86-64 --target=sse4-i32x4,avx2-i32x8,avx512skx-x16)
    set(BC7E_OBJECTS ${BC7E_INCLUDE_DIR}/bc7e${_bc7e_ext}
                     ${BC7E_INCLUDE_DIR}/bc7e_sse4${_bc7e_ext}
                     ${BC7E_INCLUDE_DIR}/bc7e_avx2${_bc7e_ext}
                     ${BC7E_INCLUDE_DIR}/bc7e_avx512skx${_bc7e_ext})
endif ()
if (NOT WIN32)
    list(APPEND _bc7e_args --pic)
endif ()

add_custom_command(
    OUTPUT ${BC7E_OBJECTS} ${BC7E_INCLUDE_DIR}/bc7e_ispc.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BC7E_INCLUDE_DIR}
    # strict IEEE maths (no --opt=fast-math): same speed/quality on x64. The old Apple Silicon
    # "pCur_ofs <= 128" assert was ispc#3882 (fixed in bc7e.ispc), not maths.
    COMMAND ${ISPC_EXECUTABLE} ${BC7E_SOURCE} -O2 --target-os=${_bc7e_os} ${_bc7e_args}
            -o ${BC7E_INCLUDE_DIR}/bc7e${_bc7e_ext} -h ${BC7E_INCLUDE_DIR}/bc7e_ispc.h
    DEPENDS ${BC7E_SOURCE}
    COMMENT "Compiling bc7e.ispc (BC7 encoder) with ISPC"
    VERBATIM)
set_source_files_properties(${BC7E_OBJECTS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
set_source_files_properties(${BC7E_INCLUDE_DIR}/bc7e_ispc.h PROPERTIES GENERATED TRUE)

# explicit build step so every generator (Makefiles, Xcode, Visual Studio) runs ISPC before
# compiling code that includes bc7e_ispc.h: add_dependencies(<target> bc7e_ispc)
add_custom_target(bc7e_ispc DEPENDS ${BC7E_OBJECTS} ${BC7E_INCLUDE_DIR}/bc7e_ispc.h)
