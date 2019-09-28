#pragma once

#if defined(__APPLE__) && defined(__has_include)
    #if __has_include(<filesystem>)
        #include <filesystem>
        namespace FileSystem = std::filesystem;
    #elif __has_include(<experimental/filesystem>)
        #include <experimental/filesystem>
        namespace FileSystem = std::experimental::filesystem;
    #else
        #include "boost/filesystem.hpp"
        namespace FileSystem = boost::filesystem;
    #endif
#else
    #include "boost/filesystem.hpp"
    namespace FileSystem = boost::filesystem;
#endif