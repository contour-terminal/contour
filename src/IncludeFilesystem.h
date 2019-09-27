#pragma once

#if !defined(__has_include) || __has_include(<filesystem>)
    #include <filesystem>
    namespace FileSystem = std::filesystem;
#elif __has_include(<experimental/filesystem>)
    #include <experimental/filesystem>
    namespace FileSystem = std::experimental::filesystem;
#elif __has_include(<boost/filesystem.hpp>)
    #include "boost/filesystem.hpp"
    namespace FileSystem = boost::filesystem;
#else
    #error No filesystem implementation found.
#endif
