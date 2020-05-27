if(APPLE)
    execute_process(
        COMMAND sh -c "sw_vers -productVersion | cut -d. -f1 | tr -d '\n'"
        OUTPUT_VARIABLE OSX_VERSION_MAJOR)
    execute_process(
        COMMAND sh -c "sw_vers -productVersion | cut -d. -f2 | tr -d '\n'"
        OUTPUT_VARIABLE OSX_VERSION_MINOR)

    if (${OSX_VERSION_MAJOR} GREATER 10 OR (${OSX_VERSION_MAJOR} EQUAL 10 AND ${OSX_VERSION_MINOR} GREATER 14))
        include(CheckCXXSymbolExists)
        CHECK_CXX_SYMBOL_EXISTS(std::experimental::filesystem::path::preferred_separator filesystem FILESYSTEM_EXPERIMENTAL_FOUND)
        CHECK_CXX_SYMBOL_EXISTS(std::filesystem::path::preferred_separator filesystem FILESYSTEM_STD_FOUND)
        if(FILESYSTEM_STD_FOUND OR FILESYSTEM_EXPERIMENTAL_FOUND)
            set(FILESYSTEM_FOUND TRUE)
        else()
            set(FILESYSTEM_FOUND FALSE)
        endif()
    else()
        message("-- OS/X is too old for std::filesystem, falling back to require Boost instead.")
        set(FILESYSTEM_FOUND FALSE)
    endif()
else()
    set(FILESYSTEM_FOUND TRUE)
endif()

if(NOT FILESYSTEM_FOUND)
    include(FindBoost)
    find_package(Boost 1.6 REQUIRED COMPONENTS filesystem)
    include_directories(${Boost_INCLUDE_DIRS})
    set(USING_BOOST TRUE)
    set(FILESYSTEM_FOUND TRUE)
endif()
