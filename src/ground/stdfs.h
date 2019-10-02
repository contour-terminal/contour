#pragma once

#if (!defined(__has_include) || __has_include(<filesystem>)) && !defined(__APPLE__)
    #include <filesystem>
    #include <system_error>
    namespace FileSystem = std::filesystem;
    typedef std::error_code FileSystemError;
#elif __has_include(<experimental/filesystem>) && !defined(__APPLE__)
    #include <experimental/filesystem>
    #include <system_error>
    namespace FileSystem = std::experimental::filesystem;
    typedef std::error_code FileSystemError;
#elif __has_include(<boost/filesystem.hpp>)
    #include "boost/filesystem.hpp"
    namespace FileSystem = boost::filesystem;
    typedef boost::system::error_code FileSystemError;
#else
    #error No filesystem implementation found.
#endif
