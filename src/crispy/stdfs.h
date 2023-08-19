// SPDX-License-Identifier: Apache-2.0
#pragma once

#if (!defined(__has_include) || __has_include(<filesystem>))
    #include <filesystem>
    #include <system_error>
namespace FileSystem = std::filesystem;
using file_system_error = std::error_code;
#elif __has_include(<experimental/filesystem>) && !defined(__APPLE__)
    #include <system_error>

    #include <experimental/filesystem>
namespace FileSystem = std::experimental::filesystem;
typedef std::error_code FileSystemError;
#else
    #error No filesystem implementation found.
#endif
