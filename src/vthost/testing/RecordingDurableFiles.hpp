// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A FileSystem over in-memory files that syncs nothing, and remembers which durability calls it was asked
/// for, in order, and what each file held when it was.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <vthost/testing/ForwardingFileSystem.hpp>

namespace vthost::testing
{

/// The durability calls of core::platform::FileSystem.
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

/// Records every durability call; fails the one @ref failing names. Everything else passes through to the
/// in-memory files.
class RecordingDurableFiles final: public ForwardingFileSystem
{
  public:
    /// The calls so far, in order, the failed one included.
    mutable std::vector<RecordedDurableCall> calls;
    /// When set, every call of this operation fails and does nothing.
    std::optional<DurableOp> failing;

    [[nodiscard]] std::expected<void, std::string> createNewFile(
        std::filesystem::path const& path) const override
    {
        return record(DurableOp::CreateExclusive, path, {}).and_then([&] {
            return ForwardingFileSystem::createNewFile(path);
        });
    }

    [[nodiscard]] std::expected<void, std::string> syncFile(std::filesystem::path const& path) const override
    {
        return record(DurableOp::SyncFile, path, files.readFile(path).value_or(std::string {}));
    }

    [[nodiscard]] std::expected<void, std::string> replaceFile(std::filesystem::path const& from,
                                                               std::filesystem::path const& to) const override
    {
        return record(DurableOp::Replace, from, files.readFile(from).value_or(std::string {})).and_then([&] {
            return ForwardingFileSystem::replaceFile(from, to);
        });
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
};

} // namespace vthost::testing
