// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// An IFilePrivacy that restricts nothing and remembers what it was asked to, and what the file held when
/// it was asked.

#include <core/platform/FileSystem.hpp>

#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <vthost/FilePrivacy.hpp>

namespace vthost::testing
{

/// One restrictToOwner() call.
struct RecordedRestrict
{
    std::filesystem::path path;
    PrivateEntry kind;

    bool operator==(RecordedRestrict const&) const = default;
};

/// Records every restriction; fails them all while @ref failure is set.
class RecordingFilePrivacy final: public IFilePrivacy
{
  public:
    /// @param fileSystem The file system whose content is sampled at each call.
    explicit RecordingFilePrivacy(core::platform::FileSystem const& fileSystem): _fileSystem { fileSystem } {}

    /// The calls so far, in order.
    mutable std::vector<RecordedRestrict> calls;
    /// When set, every call fails with this text and restricts nothing.
    std::optional<std::string> failure;

    /// @param path A path that was restricted.
    /// @return What the file held at the last call for it; empty for a directory or a missing file.
    [[nodiscard]] std::string contentAtRestrict(std::filesystem::path const& path) const
    {
        auto const found = _contents.find(path);
        return found == _contents.end() ? std::string {} : found->second;
    }

    [[nodiscard]] std::expected<void, std::string> restrictToOwner(std::filesystem::path const& path,
                                                                   PrivateEntry kind) const override
    {
        if (failure)
            return std::unexpected(*failure);
        calls.push_back({ path, kind });
        _contents[path] = _fileSystem.readFile(path).value_or(std::string {});
        return {};
    }

  private:
    core::platform::FileSystem const& _fileSystem;
    mutable std::map<std::filesystem::path, std::string> _contents;
};

} // namespace vthost::testing
