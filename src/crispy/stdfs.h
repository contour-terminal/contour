/**
 * This file is part of the "contour" project.
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#if (!defined(__has_include) || __has_include(<filesystem>))
    #include <filesystem>
    #include <system_error>
namespace FileSystem = std::filesystem;
typedef std::error_code FileSystemError;
#elif defined(USING_BOOST_FILESYSTEM) && (USING_BOOST_FILESYSTEM)
    #include <boost/filesystem.hpp>
namespace FileSystem = boost::filesystem;
typedef boost::system::error_code FileSystemError;
#elif __has_include(<experimental/filesystem>) && !defined(__APPLE__)
    #include <system_error>

    #include <experimental/filesystem>
namespace FileSystem = std::experimental::filesystem;
typedef std::error_code FileSystemError;
#elif __has_include(<boost/filesystem.hpp>)
    #include <boost/filesystem.hpp>
namespace FileSystem = boost::filesystem;
typedef boost::system::error_code FileSystemError;
#else
    #error No filesystem implementation found.
#endif
