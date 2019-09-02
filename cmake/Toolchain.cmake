# setting defaults
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/toolchains/cxx17.cmake"
        CACHE FILEPATH "Path to CMake toolchain file")
endif()
message(STATUS "Using toolchain file \"${CMAKE_TOOLCHAIN_FILE}\".")
