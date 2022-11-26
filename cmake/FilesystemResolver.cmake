option(USE_BOOST_FILESYSTEM "Compiles and links against boost::filesystem instead of std::filesystem [default: OFF]." OFF)

if(${USE_BOOST_FILESYSTEM})
    include(FindBoost)
    find_package(Boost 1.6 REQUIRED COMPONENTS filesystem)
    include_directories(${Boost_INCLUDE_DIRS})
    set(USING_BOOST_FILESYSTEM TRUE PARENT_SCOPE)
    set(USING_BOOST_FILESYSTEM TRUE)
    set(FILESYSTEM_LIBS "Boost::filesystem")
    message(STATUS "[FilesystemResolver]: Using boost::filesystem API")
else()
    set(USING_BOOST_FILESYSTEM FALSE PARENT_SCOPE)
    set(USING_BOOST_FILESYSTEM FALSE)
    message(STATUS "[FilesystemResolver]: Using standard C++ filesystem API")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION MATCHES "8.")
        # With respect to the supported operating systems, especially Ubuntu 18.04 Linux with GCC 8 needs this.
        # Newer C++ compilers won't need that but still compile if ithe respective linker flag is provided.
        # TODO: Remove this check once Ubuntu 18.04 LTS won't bee needed anymore.
        set(FILESYSTEM_LIBS "stdc++fs")
    else()
        set(FILESYSTEM_LIBS)
    endif()
endif()

