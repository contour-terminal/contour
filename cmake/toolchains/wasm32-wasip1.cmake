# The toolchain of Contour's C++ WebAssembly guests: C++23 for wasm32-wasip1, linked as a WASI reactor.
#
# It finds nothing by itself: cmake/ContourWasiCxx.cmake detects the toolchain once, in the host build, and
# hands every guest sub-build what it found:
#
#   CONTOUR_WASI_C_COMPILER    clang
#   CONTOUR_WASI_CXX_COMPILER  clang++
#   CONTOUR_WASI_LINKER        wasm-ld, passed to clang as --ld-path
#   CONTOUR_WASI_AR            llvm-ar, and CONTOUR_WASI_RANLIB llvm-ranlib, which index wasm objects
#   CONTOUR_WASI_SYSROOT       the sysroot holding wasi-libc and libc++
#   CONTOUR_WASI_CMAKE_DIR     optional: a directory with Platform/WASI.cmake, for a CMake that lacks it
#
# A guest is a reactor (no `main`, `_initialize` runs the constructors) with neither exceptions nor RTTI, the
# way the host loads it; --strip-debug, not --strip-all, so that custom sections such as the extension
# manifest survive the link.

foreach(_required IN ITEMS CONTOUR_WASI_C_COMPILER CONTOUR_WASI_CXX_COMPILER CONTOUR_WASI_LINKER CONTOUR_WASI_AR
                           CONTOUR_WASI_RANLIB CONTOUR_WASI_SYSROOT)
    if(NOT ${_required})
        message(FATAL_ERROR "wasm32-wasip1 toolchain: ${_required} is not set; include ContourWasiCxx.cmake instead.")
    endif()
endforeach()

# The values above reach the configurations CMake runs for its own checks too.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
     CONTOUR_WASI_C_COMPILER CONTOUR_WASI_CXX_COMPILER CONTOUR_WASI_LINKER CONTOUR_WASI_AR CONTOUR_WASI_RANLIB
     CONTOUR_WASI_SYSROOT CONTOUR_WASI_CMAKE_DIR)

if(CONTOUR_WASI_CMAKE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${CONTOUR_WASI_CMAKE_DIR}")
endif()

set(CMAKE_SYSTEM_NAME WASI)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR wasm32)
# Per language: CMakeGenericSystem.cmake resets CMAKE_EXECUTABLE_SUFFIX after a toolchain file has set it.
set(CMAKE_EXECUTABLE_SUFFIX_C ".wasm")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".wasm")

set(CMAKE_C_COMPILER "${CONTOUR_WASI_C_COMPILER}")
set(CMAKE_CXX_COMPILER "${CONTOUR_WASI_CXX_COMPILER}")
set(CMAKE_AR "${CONTOUR_WASI_AR}")
set(CMAKE_RANLIB "${CONTOUR_WASI_RANLIB}")
set(CMAKE_C_COMPILER_TARGET wasm32-wasip1)
set(CMAKE_CXX_COMPILER_TARGET wasm32-wasip1)
set(CMAKE_SYSROOT "${CONTOUR_WASI_SYSROOT}")

set(CMAKE_C_FLAGS_INIT "-fvisibility=hidden")
set(CMAKE_CXX_FLAGS_INIT "-fno-exceptions -fno-rtti -fvisibility=hidden")
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "--ld-path=${CONTOUR_WASI_LINKER} -mexec-model=reactor -Wl,--gc-sections -Wl,--strip-debug -Wl,-z,stack-size=65536")

# Programs run during the build are the host's; everything else comes from the sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
