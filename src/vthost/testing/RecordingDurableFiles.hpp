// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// An IDurableFiles over a FileSystem that syncs nothing, creates and renames through the file system, and
/// remembers what
/// it was asked to do, in order, and what each file held when it was.

#include <core/platform/FileSystem.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <vthost/DurableFiles.hpp>

namespace vthost::testing
{

/// The calls of IDurableFiles.
enum class DurableOp : std::uint8_t
{
    CreateExclusive = 0,
    SyncFile,
    Replace,
    SyncDirectory,
};

/// One call.
struct RecordedDurableCall
{
    DurableOp op;
    std::filesystem::path path; ///< The file synced, the file renamed, or the directory synced.
    std::string content {};     ///< What the file held at the call; empty for a directory.

    bool operator==(RecordedDurableCall const&) const = default;
};

/// Records every call; fails the one @ref failing names.
class RecordingDurableFiles final: public IDurableFiles
{
  public:
    /// @param fileSystem The file system renamed in and sampled at each call.
    explicit RecordingDurableFiles(core::platform::FileSystem const& fileSystem): _fileSystem { fileSystem }
    {
    }

    /// The calls so far, in order, the failed one included.
    mutable std::vector<RecordedDurableCall> calls;
    /// When set, every call of this operation fails and does nothing.
    std::optional<DurableOp> failing;

    [[nodiscard]] std::expected<void, std::string> createExclusive(
        std::filesystem::path const& path) const override
    {
        return record(DurableOp::CreateExclusive, path, {})
            .and_then([&]() -> std::expected<void, std::string> {
                if (_fileSystem.exists(path))
                    return std::unexpected("File exists: " + path.string());
                return _fileSystem.writeFile(path, {});
            });
    }

    [[nodiscard]] std::expected<void, std::string> syncFile(std::filesystem::path const& path) const override
    {
        return record(DurableOp::SyncFile, path, _fileSystem.readFile(path).value_or(std::string {}));
    }

    [[nodiscard]] std::expected<void, std::string> replace(std::filesystem::path const& from,
                                                           std::filesystem::path const& to) const override
    {
        return record(DurableOp::Replace, from, _fileSystem.readFile(from).value_or(std::string {}))
            .and_then([&] { return _fileSystem.rename(from, to); });
    }

    [[nodiscard]] std::expected<void, std::string> syncDirectory(
        std::filesystem::path const& directory) const override
    {
        return record(DurableOp::SyncDirectory, directory, {});
    }

  private:
    [[nodiscard]] std::expected<void, std::string> record(DurableOp op,
                                                          std::filesystem::path const& path,
                                                          std::string content) const
    {
        calls.push_back({ .op = op, .path = path, .content = std::move(content) });
        if (failing == op)
            return std::unexpected("Input/output error: " + path.string());
        return {};
    }

    core::platform::FileSystem const& _fileSystem;
};

} // namespace vthost::testing
