// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/FileInfoProvider.hpp>
#include <core/platform/GlobMatch.hpp>

#include <map>
#include <string>
#include <utility>

namespace core::platform::testing
{

/// Mock FileInfoProvider for unit testing.
///
/// Returns configurable directory listings keyed by path.
/// Supports single-file lookups and basic glob pattern filtering.
class MockFileInfoProvider final: public FileInfoProvider
{
  public:
    /// Sets the entries to return for a given directory path.
    void setEntries(std::string const& path, std::vector<core::platform::FileEntry> entries)
    {
        _directories[path] = std::move(entries);
    }

    /// Sets a single file entry to return for a given file path.
    void setFileEntry(std::string const& path, core::platform::FileEntry entry)
    {
        _files[path] = std::move(entry);
    }

    [[nodiscard]] std::vector<core::platform::FileEntry> listDirectory(std::string const& path) const override
    {
        // Case 1: Exact directory match.
        if (auto const it = _directories.find(path); it != _directories.end())
            return it->second;

        // Case 2: Exact single-file match.
        if (auto const it = _files.find(path); it != _files.end())
            return { it->second };

        // Case 3: Glob pattern — find parent directory entries and filter.
        if (containsGlobChars(path))
        {
            // Extract parent directory and filename pattern.
            auto const lastSlash = path.find_last_of('/');
            auto const parentDir =
                (lastSlash != std::string::npos) ? path.substr(0, lastSlash) : std::string(".");
            auto const pattern = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;

            if (auto const dirIt = _directories.find(parentDir); dirIt != _directories.end())
            {
                std::vector<core::platform::FileEntry> result;
                for (auto const& entry: dirIt->second)
                {
                    if (globMatchFilename(entry.name, pattern))
                        result.push_back(entry);
                }
                return result;
            }
        }

        return {};
    }

  private:
    std::map<std::string, std::vector<core::platform::FileEntry>> _directories;
    std::map<std::string, core::platform::FileEntry> _files;
};

} // namespace core::platform::testing
