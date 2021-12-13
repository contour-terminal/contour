# This directory structure is being created by `scripts/install-deps.sh`
# and is used to inject all the dependencies the operating system's
# package manager did not provide (not found or too old version).

if(EXISTS ${PROJECT_SOURCE_DIR}/_deps/sources/CMakeLists.txt)
    message(STATUS "Embedding 3rdparty libraries ...")
    add_subdirectory(${PROJECT_SOURCE_DIR}/_deps/sources)
endif()

# Now, conditionally find all dependencies that were not included above
# via find_package, usually system installed packages.

if (NOT TARGET Catch2::Catch2)
    find_package(Catch2 REQUIRED)
endif()

if(NOT TARGET fmt)
    find_package(fmt REQUIRED)
endif()

if(NOT TARGET GSL)
    find_package(GSL REQUIRED)
endif()

if (NOT TARGET range-v3)
    find_package(range-v3 REQUIRED)
endif()

if (NOT TARGET termbench)
    find_package(termbench-pro REQUIRED)
endif()

if (NOT TARGET unicode::core)
    find_package(libunicode REQUIRED)
endif()

if (NOT TARGET yaml-cpp)
    find_package(yaml-cpp REQUIRED)
endif()
