cmake_minimum_required(VERSION 3.13)
#project(libground VERSION "0.0.0" LANGUAGES CXX)

include(FilesystemResolver)

option(LIBGROUND_TESTING "Enables building of unittests for libground [default: ON]" ON)

if(MSVC)
    add_definitions(-DNOMINMAX)
endif()

add_library(ground STATIC
    StringUtils.cpp StringUtils.h
    FileChangeWatcher.cpp FileChangeWatcher.h
    overloaded.h
    stdfs.h
)
target_include_directories(ground PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)

if(Boost_FILESYSTEM_FOUND)
    target_link_libraries(ground PRIVATE ${Boost_FILESYSTEM_LIBRARY})
endif()

# ----------------------------------------------------------------------------
if(LIBGROUND_TESTING)
    enable_testing()
    add_executable(ground_test
        StringUtils_test.cpp
    )
    target_link_libraries(ground_test fmt::fmt-header-only Catch2::Catch2 ground)
    add_test(ground_test ./ground_test)
endif(LIBGROUND_TESTING)
